// Chat overlay mode for native_vp8_stream.exe
//
// Runs as:
//   native_vp8_stream.exe --mode chat-overlay --session <id> --pipe-in <svc_to_ui> --pipe-out <ui_to_svc> [--stop-event <Global\event>]
//
// Uses two named pipes so service->overlay and overlay->service never block each other.
// WebView2 user data is placed in a temporary per-session folder and cleaned up on exit.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "util/log.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace hi5 {
    namespace {

        constexpr UINT WM_HI5_PIPE_MESSAGE = WM_APP + 410;
        constexpr UINT WM_HI5_SHOW_READY = WM_APP + 411;

        struct ChatMessage {
            std::int64_t id = 0;
            std::string sender;
            std::string displayName;
            std::string body;
            std::int64_t unixMs = 0;
        };

        struct AppState {
            HINSTANCE hInstance = nullptr;
            HWND hwnd = nullptr;
            std::string sessionId;
            std::wstring pipeInNameW;   // service -> overlay, overlay reads
            std::wstring pipeOutNameW;  // overlay -> service, overlay writes
            HANDLE pipeIn = INVALID_HANDLE_VALUE;
            HANDLE pipeOut = INVALID_HANDLE_VALUE;
            HANDLE stopEvent = nullptr;
            std::wstring stopEventNameW;
            std::atomic<bool> stop{ false };
            std::thread pipeThread;
            std::thread stopEventThread;

            std::mutex mu;
            std::vector<ChatMessage> backlog;
            std::vector<std::string> pendingToService;
            bool outConnected = false;
            std::int64_t nextId = 1;

            ComPtr<ICoreWebView2Controller> controller;
            ComPtr<ICoreWebView2> webview;
            bool webReady = false;
            std::int64_t lastPostedId = 0;
            std::filesystem::path userDataFolder;
        };

        static AppState* g_app = nullptr;

        static void LogI(const std::string& s) { LogInfo("[chat-overlay] " + s); }
        static void LogW(const std::string& s) { LogWarn("[chat-overlay] " + s); }
        static void LogE(const std::string& s) { LogError("[chat-overlay] " + s); }

        static std::int64_t NowUnixMs() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        static std::wstring ToWide(const std::string& s) {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) {
                n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
                if (n <= 0) return {};
                std::wstring w(n, L'\0');
                MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
                return w;
            }
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        static std::string FromWide(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0) return {};
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        static std::string TrimCopy(std::string s) {
            auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        static std::string SafeFilePart(std::string value) {
            for (char& c : value) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
                if (!ok) c = '_';
            }
            if (value.empty()) value = "session";
            return value;
        }

        static std::string BodyFromJson(const json& j) {
            for (const char* key : { "body", "message", "text", "content" }) {
                auto it = j.find(key);
                if (it != j.end() && it->is_string()) return it->get<std::string>();
            }
            return {};
        }

        static bool WriteLine(HANDLE pipe, const std::string& line) {
            if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;
            std::string data = line;
            if (data.empty() || data.back() != '\n') data.push_back('\n');
            DWORD written = 0;
            BOOL ok = WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            FlushFileBuffers(pipe);
            return ok && written == data.size();
        }

        static void FlushPendingToService() {
            if (!g_app || g_app->pipeOut == INVALID_HANDLE_VALUE) return;
            std::vector<std::string> copy;
            {
                std::lock_guard<std::mutex> lock(g_app->mu);
                if (!g_app->outConnected) return;
                copy.swap(g_app->pendingToService);
            }
            for (const auto& line : copy) {
                if (!WriteLine(g_app->pipeOut, line)) {
                    LogE("failed to flush pending reply to service err=" + std::to_string(GetLastError()));
                    std::lock_guard<std::mutex> lock(g_app->mu);
                    g_app->pendingToService.insert(g_app->pendingToService.begin(), line);
                    break;
                }
            }
        }

        static bool QueueOrWriteToService(const std::string& line) {
            if (!g_app) return false;
            {
                std::lock_guard<std::mutex> lock(g_app->mu);
                if (!g_app->outConnected || g_app->pipeOut == INVALID_HANDLE_VALUE) {
                    g_app->pendingToService.push_back(line);
                    LogW("service output pipe not connected yet; queued reply count=" + std::to_string(g_app->pendingToService.size()));
                    return true;
                }
            }
            if (!WriteLine(g_app->pipeOut, line)) {
                LogE("failed to write user reply to service output pipe err=" + std::to_string(GetLastError()));
                std::lock_guard<std::mutex> lock(g_app->mu);
                g_app->pendingToService.push_back(line);
                return false;
            }
            return true;
        }

        static HANDLE OpenPipeClient(const std::wstring& pipeName, DWORD access, const char* label) {
            for (int i = 0; i < 120 && !g_app->stop.load(); ++i) {
                HANDLE h = CreateFileW(pipeName.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD mode = PIPE_READMODE_MESSAGE;
                    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                    LogI(std::string(label) + " pipe connected");
                    return h;
                }
                const DWORD err = GetLastError();
                if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
                    LogW(std::string(label) + " pipe connect retry err=" + std::to_string(err));
                }
                WaitNamedPipeW(pipeName.c_str(), 250);
                Sleep(75);
            }
            LogE(std::string(label) + " pipe connect failed");
            return INVALID_HANDLE_VALUE;
        }

        static json MessageToJson(const ChatMessage& m) {
            return json{
                {"id", m.id},
                {"type", "chat_message"},
                {"session_id", g_app ? g_app->sessionId : std::string()},
                {"sender", m.sender},
                {"display_name", m.displayName},
                {"body", m.body},
                {"unix_ms", m.unixMs}
            };
        }

        static void PostMessageToWebView(const ChatMessage& m) {
            if (!g_app || !g_app->webview || !g_app->webReady) return;
            const std::wstring payload = ToWide(MessageToJson(m).dump());
            g_app->webview->PostWebMessageAsJson(payload.c_str());
        }

        static void FlushBacklogToWebView() {
            if (!g_app || !g_app->webview || !g_app->webReady) return;
            std::vector<ChatMessage> copy;
            {
                std::lock_guard<std::mutex> lock(g_app->mu);
                copy = g_app->backlog;
            }
            for (const auto& m : copy) {
                if (m.id > g_app->lastPostedId) {
                    PostMessageToWebView(m);
                    g_app->lastPostedId = m.id;
                }
            }
        }

        static void AddIncomingMessage(const std::string& sender,
            const std::string& displayName,
            const std::string& body,
            std::int64_t unixMs) {
            std::string clean = TrimCopy(body);
            if (clean.empty() || !g_app) return;

            ChatMessage m;
            {
                std::lock_guard<std::mutex> lock(g_app->mu);
                m.id = g_app->nextId++;
                m.sender = sender.empty() ? "tech" : sender;
                m.displayName = displayName.empty() ? (m.sender == "user" ? "You" : "Technician") : displayName;
                m.body = clean;
                m.unixMs = unixMs > 0 ? unixMs : NowUnixMs();
                g_app->backlog.push_back(m);
            }
            PostMessageW(g_app->hwnd, WM_HI5_PIPE_MESSAGE, 0, 0);
        }

        static void SendUserReplyToService(const std::string& body) {
            std::string clean = TrimCopy(body);
            if (clean.empty() || !g_app) return;

            ChatMessage local;
            {
                std::lock_guard<std::mutex> lock(g_app->mu);
                local.id = g_app->nextId++;
                local.sender = "user";
                local.displayName = "You";
                local.body = clean;
                local.unixMs = NowUnixMs();
                g_app->backlog.push_back(local);
            }
            PostMessageW(g_app->hwnd, WM_HI5_PIPE_MESSAGE, 0, 0);

            json outgoing = {
                {"type", "chat_message"},
                {"session_id", g_app->sessionId},
                {"sender", "user"},
                {"display_name", "Remote user"},
                {"body", clean},
                {"unix_ms", local.unixMs}
            };
            const bool wrote = QueueOrWriteToService(outgoing.dump());
            if (wrote) {
                LogI("user reply queued/written to service output pipe bytes=" + std::to_string(clean.size()));
            }
        }

        static void PipeClientThread() {
            if (!g_app) return;

            // Connect the two directions independently. In v6 this was sequential, so
            // one slow/missing pipe could block the other side and the UI looked dead.
            std::thread outThread([]() {
                if (!g_app) return;
                g_app->pipeOut = OpenPipeClient(g_app->pipeOutNameW, GENERIC_WRITE, "output");
                if (g_app->pipeOut != INVALID_HANDLE_VALUE) {
                    {
                        std::lock_guard<std::mutex> lock(g_app->mu);
                        g_app->outConnected = true;
                    }
                    WriteLine(g_app->pipeOut, json{ {"type", "overlay_ready"}, {"session_id", g_app->sessionId} }.dump());
                    LogI("overlay_ready sent");
                    FlushPendingToService();
                }
                else {
                    LogW("output pipe unavailable; replies will remain queued locally until next launch");
                }
                });

            g_app->pipeIn = OpenPipeClient(g_app->pipeInNameW, GENERIC_READ, "input");
            if (g_app->pipeIn == INVALID_HANDLE_VALUE) {
                LogW("input pipe unavailable; closing chat overlay");
                g_app->stop.store(true);
                if (g_app->hwnd) PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                if (outThread.joinable()) outThread.join();
                return;
            }

            std::string pending;
            char buf[8192];
            while (!g_app->stop.load()) {
                DWORD got = 0;
                BOOL ok = ReadFile(g_app->pipeIn, buf, sizeof(buf), &got, nullptr);
                if (!ok || got == 0) {
                    DWORD err = GetLastError();
                    if (!g_app->stop.load()) LogW("input pipe read stopped err=" + std::to_string(err));
                    g_app->stop.store(true);
                    if (g_app->hwnd) PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                    break;
                }
                pending.append(buf, buf + got);
                for (;;) {
                    auto pos = pending.find('\n');
                    if (pos == std::string::npos) break;
                    std::string line = pending.substr(0, pos);
                    pending.erase(0, pos + 1);
                    auto j = json::parse(line, nullptr, false);
                    if (j.is_discarded()) {
                        LogW("discarded invalid JSON from input pipe");
                        continue;
                    }
                    const std::string type = j.value("type", "");
                    if (type == "chat_message") {
                        const std::string sender = j.value("sender", "tech");
                        if (sender == "user") continue;
                        const std::string body = BodyFromJson(j);
                        LogI("received technician message from service bytes=" + std::to_string(body.size()));
                        AddIncomingMessage(sender,
                            j.value("display_name", std::string("Technician")),
                            body,
                            j.value("unix_ms", static_cast<std::int64_t>(0)));
                    }
                    else if (type == "close" || type == "session_ended") {
                        LogI("close received from service");
                        PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                    }
                }
            }

            if (outThread.joinable()) outThread.join();
        }

        static std::filesystem::path TempRoot() {
            wchar_t temp[MAX_PATH]{};
            DWORD n = GetTempPathW(MAX_PATH, temp);
            std::filesystem::path root = n ? std::filesystem::path(temp) : std::filesystem::temp_directory_path();
            return root / L"Hi5Central" / L"ChatWebView2";
        }

        static void CleanupOldTempFolders() {
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto root = TempRoot();
            fs::create_directories(root, ec);
            const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
            for (const auto& item : fs::directory_iterator(root, ec)) {
                if (ec) break;
                const auto t = item.last_write_time(ec);
                if (!ec && t < cutoff) fs::remove_all(item.path(), ec);
            }
        }

        static std::wstring HtmlPage() {
            return LR"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<style>
:root { color-scheme: light dark; --bg: rgba(246,248,252,.97); --surface: rgba(255,255,255,.82); --line: rgba(15,23,42,.10); --text: #111827; --muted: #64748b; --tech: #ffffff; --user: linear-gradient(135deg,#2563eb,#7c3aed); --userText: #ffffff; --shadow: 0 22px 55px rgba(15,23,42,.25); }
@media (prefers-color-scheme: dark) { :root { --bg: rgba(15,23,42,.97); --surface: rgba(30,41,59,.80); --line: rgba(255,255,255,.11); --text: #f8fafc; --muted: #94a3b8; --tech: rgba(51,65,85,.96); --shadow: 0 24px 70px rgba(0,0,0,.46); } }
* { box-sizing: border-box; } html, body { width:100%; height:100%; margin:0; overflow:hidden; background:transparent; } body { font-family:"Segoe UI", system-ui, -apple-system, BlinkMacSystemFont, sans-serif; color:var(--text); }
.app { width:100vw; height:100vh; display:flex; flex-direction:column; background:var(--bg); border:1px solid var(--line); border-radius:22px; overflow:hidden; box-shadow:var(--shadow); }
.header { height:76px; display:flex; align-items:center; gap:13px; padding:16px 18px; border-bottom:1px solid var(--line); background:linear-gradient(180deg,rgba(255,255,255,.30),rgba(255,255,255,.04)); user-select:none; }
.badge { width:42px; height:42px; border-radius:14px; display:grid; place-items:center; color:#fff; font-weight:850; letter-spacing:.25px; background:linear-gradient(135deg,#2563eb,#7c3aed); box-shadow:0 12px 24px rgba(37,99,235,.34); }
.title { flex:1; min-width:0; cursor:default; } .title h1 { margin:0; font-size:16px; line-height:22px; font-weight:760; } .title p { margin:1px 0 0; color:var(--muted); font-size:12px; }
.close { border:0; border-radius:13px; width:36px; height:36px; color:var(--muted); background:transparent; font-size:20px; line-height:1; cursor:pointer; } .close:hover { background:rgba(148,163,184,.18); color:var(--text); }
.messages { flex:1; overflow:auto; padding:18px; display:flex; flex-direction:column; gap:12px; } .empty { margin:auto; color:var(--muted); text-align:center; font-size:14px; }
.row { display:flex; flex-direction:column; gap:4px; max-width:82%; animation:pop .18s ease-out; } .row.tech { align-self:flex-start; } .row.user { align-self:flex-end; align-items:flex-end; }
.name { font-size:11px; color:var(--muted); padding:0 4px; } .bubble { padding:11px 13px; border-radius:18px; font-size:14px; line-height:1.4; white-space:pre-wrap; word-break:break-word; border:1px solid var(--line); }
.tech .bubble { background:var(--tech); border-top-left-radius:6px; } .user .bubble { background:var(--user); color:var(--userText); border-color:transparent; border-top-right-radius:6px; }
.composer { padding:12px; display:flex; gap:10px; border-top:1px solid var(--line); background:var(--surface); } .input { flex:1; min-height:42px; max-height:92px; resize:none; border:1px solid var(--line); border-radius:16px; padding:11px 13px; outline:none; background:rgba(255,255,255,.62); color:var(--text); font:inherit; } @media (prefers-color-scheme: dark) { .input { background:rgba(15,23,42,.72); } }
.send { border:0; border-radius:16px; padding:0 18px; min-width:76px; color:white; font-weight:730; cursor:pointer; background:linear-gradient(135deg,#2563eb,#7c3aed); box-shadow:0 10px 18px rgba(37,99,235,.24); } .send:disabled { opacity:.45; cursor:not-allowed; box-shadow:none; }
@keyframes pop { from { transform:translateY(5px); opacity:0; } to { transform:none; opacity:1; } }
</style>
</head>
<body>
<div class="app"><div id="drag" class="header"><div class="badge">H5</div><div class="title"><h1>Hi5Central Support Chat</h1><p>A technician is connected</p></div><button id="close" class="close" title="Close">X</button></div><div id="messages" class="messages"><div class="empty">Waiting for the technician...</div></div><div class="composer"><textarea id="input" class="input" rows="1" placeholder="Type a reply..."></textarea><button id="send" class="send">Send</button></div></div>
<script>
const messages=document.getElementById('messages'),input=document.getElementById('input'),send=document.getElementById('send'),closeBtn=document.getElementById('close'),drag=document.getElementById('drag');
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e;}
function render(m){if(!m)return;const empty=messages.querySelector('.empty');if(empty)empty.remove();const mine=m.sender==='user';const row=el('div','row '+(mine?'user':'tech'));row.appendChild(el('div','name',m.display_name||(mine?'You':'Technician')));row.appendChild(el('div','bubble',m.body||'[message received]'));messages.appendChild(row);messages.scrollTop=messages.scrollHeight;}
function post(obj){try{if(window.chrome&&window.chrome.webview){window.chrome.webview.postMessage(obj);return true;}}catch(e){} return false;}
function doSend(){const body=input.value.trim();if(!body)return;input.value='';post({type:'send_message',body});input.focus();}
send.addEventListener('click',doSend);input.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();doSend();}});
closeBtn.addEventListener('click',()=>{if(confirm('Are you sure you want to close this support chat?'))post({type:'close_requested'});});
drag.addEventListener('mousedown',e=>{if(e.button===0&&(e.target===drag||e.target.closest('.title')||e.target.classList.contains('badge')))post({type:'drag_window'});});
if(window.chrome&&window.chrome.webview){window.chrome.webview.addEventListener('message',e=>render(e.data));post({type:'web_ready'});} setTimeout(()=>input.focus(),250);
</script>
</body>
</html>)HTML";
        }

        static void ResizeWebView() {
            if (!g_app || !g_app->controller || !g_app->hwnd) return;
            RECT bounds{};
            GetClientRect(g_app->hwnd, &bounds);
            g_app->controller->put_Bounds(bounds);
        }

        static void InitWebView2() {
            const std::wstring userData = g_app ? g_app->userDataFolder.wstring() : std::wstring();
            LogI("using temp WebView2 user data folder=" + FromWide(userData));
            HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr,
                userData.empty() ? nullptr : userData.c_str(),
                nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                        if (FAILED(result) || !env) {
                            LogE("CreateCoreWebView2EnvironmentWithOptions failed hr=" + std::to_string(static_cast<long>(result)));
                            MessageBoxW(nullptr, L"Microsoft WebView2 Runtime is required for Hi5Central chat.", L"Hi5Central", MB_ICONERROR | MB_OK);
                            PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                            return S_OK;
                        }
                        env->CreateCoreWebView2Controller(
                            g_app->hwnd,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (FAILED(result) || !controller) {
                                        LogE("CreateCoreWebView2Controller failed hr=" + std::to_string(static_cast<long>(result)));
                                        PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                                        return S_OK;
                                    }
                                    g_app->controller = controller;
                                    controller->get_CoreWebView2(&g_app->webview);
                                    ResizeWebView();
                                    ComPtr<ICoreWebView2Settings> settings;
                                    if (SUCCEEDED(g_app->webview->get_Settings(&settings)) && settings) {
                                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                                        settings->put_AreDevToolsEnabled(FALSE);
                                        settings->put_IsStatusBarEnabled(FALSE);
                                        settings->put_IsZoomControlEnabled(FALSE);
                                    }
                                    EventRegistrationToken token{};
                                    g_app->webview->add_WebMessageReceived(
                                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                LPWSTR raw = nullptr;
                                                if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) {
                                                    if (raw) CoTaskMemFree(raw);
                                                    return S_OK;
                                                }
                                                std::wstring w(raw);
                                                CoTaskMemFree(raw);
                                                auto j = json::parse(FromWide(w), nullptr, false);
                                                if (j.is_discarded()) return S_OK;
                                                const std::string type = j.value("type", "");
                                                if (type == "web_ready") {
                                                    g_app->webReady = true;
                                                    FlushBacklogToWebView();
                                                }
                                                else if (type == "send_message") {
                                                    SendUserReplyToService(BodyFromJson(j));
                                                }
                                                else if (type == "drag_window") {
                                                    ReleaseCapture();
                                                    SendMessageW(g_app->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                                }
                                                else if (type == "close_requested") {
                                                    PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                                                }
                                                return S_OK;
                                            }).Get(),
                                                &token);
                                    g_app->webview->NavigateToString(HtmlPage().c_str());
                                    PostMessageW(g_app->hwnd, WM_HI5_SHOW_READY, 0, 0);
                                    return S_OK;
                                }).Get());
                        return S_OK;
                    }).Get());
            if (FAILED(hr)) {
                LogE("CreateCoreWebView2EnvironmentWithOptions immediate failure hr=" + std::to_string(static_cast<long>(hr)));
                MessageBoxW(nullptr, L"Microsoft WebView2 Runtime is required for Hi5Central chat.", L"Hi5Central", MB_ICONERROR | MB_OK);
                PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
            }
        }

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            switch (msg) {
            case WM_CREATE:
                return 0;
            case WM_SIZE:
                ResizeWebView();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_HI5_PIPE_MESSAGE:
                FlushBacklogToWebView();
                return 0;
            case WM_HI5_SHOW_READY:
                ShowWindow(hwnd, SW_SHOWNORMAL);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                UpdateWindow(hwnd);
                return 0;
            case WM_CLOSE:
                if (g_app) g_app->stop.store(true);
                if (g_app && g_app->pipeOut != INVALID_HANDLE_VALUE) {
                    WriteLine(g_app->pipeOut, json{ {"type", "closed"}, {"session_id", g_app->sessionId} }.dump());
                }
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
        }

        static bool ParseArgs(int argc, char** argv, std::string& sessionId, std::wstring& pipeInW, std::wstring& pipeOutW, std::wstring& stopEventW) {
            std::string inPipe;
            std::string outPipe;
            std::string stopEvent;
            for (int i = 1; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--session" && i + 1 < argc) sessionId = argv[++i];
                else if (a == "--pipe-in" && i + 1 < argc) inPipe = argv[++i];
                else if (a == "--pipe-out" && i + 1 < argc) outPipe = argv[++i];
                else if (a == "--stop-event" && i + 1 < argc) stopEvent = argv[++i];
                else if (a == "--pipe" && i + 1 < argc) { inPipe = argv[++i]; outPipe = inPipe; }
            }
            if (sessionId.empty() || inPipe.empty() || outPipe.empty()) return false;
            pipeInW = ToWide(inPipe);
            pipeOutW = ToWide(outPipe);
            stopEventW = ToWide(stopEvent);
            return !pipeInW.empty() && !pipeOutW.empty();
        }

    } // namespace

    int RunChatOverlayMain(int argc, char** argv) {
        LogI("chat overlay mode start");

        AppState app;
        g_app = &app;
        app.hInstance = GetModuleHandleW(nullptr);

        if (!ParseArgs(argc, argv, app.sessionId, app.pipeInNameW, app.pipeOutNameW, app.stopEventNameW)) {
            LogE("missing --session, --pipe-in or --pipe-out args");
            return 2;
        }

        if (!app.stopEventNameW.empty()) {
            app.stopEvent = OpenEventW(SYNCHRONIZE, FALSE, app.stopEventNameW.c_str());
            if (!app.stopEvent) {
                LogW("failed to open stop event err=" + std::to_string(GetLastError()));
            }
        }

        CleanupOldTempFolders();
        std::error_code ec;
        app.userDataFolder = TempRoot() / ToWide(SafeFilePart(app.sessionId));
        std::filesystem::create_directories(app.userDataFolder, ec);

        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(co)) {
            LogE("CoInitializeEx failed hr=" + std::to_string(static_cast<long>(co)));
            return 3;
        }

        const wchar_t* className = L"Hi5CentralChatOverlayWindowV8";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = app.hInstance;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = 460;
        const int height = 620;
        const int x = std::max<LONG>(work.left + 16, work.right - width - 28);
        const int y = std::max<LONG>(work.top + 16, work.bottom - height - 28);

        DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        DWORD style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        app.hwnd = CreateWindowExW(exStyle, className, L"Hi5Central Support Chat", style,
            x, y, width, height, nullptr, nullptr, app.hInstance, nullptr);
        if (!app.hwnd) {
            LogE("CreateWindowExW failed err=" + std::to_string(GetLastError()));
            CoUninitialize();
            return 4;
        }

        BOOL dark = TRUE;
        DwmSetWindowAttribute(app.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

        // Show the shell immediately. WebView2 initialisation and pipe connections
        // are asynchronous and must not be able to block the visible chat window.
        ShowWindow(app.hwnd, SW_SHOWNORMAL);
        SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(app.hwnd);
        LogI("chat overlay window shown; starting WebView2 and pipe threads");

        InitWebView2();
        app.pipeThread = std::thread(PipeClientThread);
        if (app.stopEvent) {
            app.stopEventThread = std::thread([]() {
                while (g_app && !g_app->stop.load()) {
                    DWORD rc = WaitForSingleObject(g_app->stopEvent, 250);
                    if (rc == WAIT_OBJECT_0) {
                        LogI("session stop event signaled; closing overlay");
                        g_app->stop.store(true);
                        if (g_app->hwnd) PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
                        break;
                    }
                }
            });
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        app.stop.store(true);
        if (app.pipeIn != INVALID_HANDLE_VALUE) {
            CancelIoEx(app.pipeIn, nullptr);
            CloseHandle(app.pipeIn);
            app.pipeIn = INVALID_HANDLE_VALUE;
        }
        if (app.pipeOut != INVALID_HANDLE_VALUE) {
            CancelIoEx(app.pipeOut, nullptr);
            CloseHandle(app.pipeOut);
            app.pipeOut = INVALID_HANDLE_VALUE;
        }
        if (app.pipeThread.joinable()) app.pipeThread.join();
        if (app.stopEventThread.joinable()) app.stopEventThread.join();
        if (app.stopEvent) {
            CloseHandle(app.stopEvent);
            app.stopEvent = nullptr;
        }

        app.webview.Reset();
        app.controller.Reset();
        CoUninitialize();

        std::filesystem::remove_all(app.userDataFolder, ec);
        LogI("chat overlay mode stopped");
        return 0;
    }

} // namespace hi5