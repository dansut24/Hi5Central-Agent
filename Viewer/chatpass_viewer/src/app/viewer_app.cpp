#include "app/viewer_app.h"

#include "core/deep_link.h"
#include "core/logger.h"

#include <webview/webview.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif

namespace hi5 {
    namespace {

        std::string JsEscape(const std::string& value) {
            std::string out;
            out.reserve(value.size() + 16);

            for (char c : value) {
                switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '<':  out += "\\x3C"; break;
                case '>':  out += "\\x3E"; break;
                default:   out.push_back(c); break;
                }
            }

            return out;
        }


        std::string GetLocalComputerLabel() {
#ifdef _WIN32
            char name[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
            if (GetComputerNameA(name, &len) && len > 0) {
                return std::string("This PC - ") + std::string(name, len);
            }
#endif
            return "This PC";
        }

        std::string ShortRemoteLabel(const std::string& deviceId) {
            if (deviceId.empty()) return "Remote device";
            const std::size_t n = std::min<std::size_t>(8, deviceId.size());
            return std::string("Remote device - ") + deviceId.substr(0, n);
        }

        std::string ReadTextFileUtf8(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return {};
            }

            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        std::string ParseFirstJsonStringArg(const std::string& req) {
            bool inString = false;
            bool escaping = false;
            std::string out;

            for (char c : req) {
                if (!inString) {
                    if (c == '"') inString = true;
                    continue;
                }

                if (escaping) {
                    switch (c) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    default: out.push_back(c); break;
                    }
                    escaping = false;
                    continue;
                }

                if (c == '\\') {
                    escaping = true;
                    continue;
                }

                if (c == '"') {
                    return out;
                }

                out.push_back(c);
            }

            return out;
        }


        std::string JsonEscape(const std::string& value) {
            std::string out;
            out.reserve(value.size() + 16);
            for (unsigned char c : value) {
                switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8]{};
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
                }
            }
            return out;
        }

        std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
            const std::string needle = "\"" + key + "\"";
            size_t pos = json.find(needle);
            if (pos == std::string::npos) return {};
            pos = json.find(':', pos + needle.size());
            if (pos == std::string::npos) return {};
            pos = json.find('"', pos + 1);
            if (pos == std::string::npos) return {};
            ++pos;
            std::string out;
            bool esc = false;
            for (; pos < json.size(); ++pos) {
                char c = json[pos];
                if (esc) {
                    switch (c) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    default: out.push_back(c); break;
                    }
                    esc = false;
                    continue;
                }
                if (c == '\\') { esc = true; continue; }
                if (c == '"') break;
                out.push_back(c);
            }
            return out;
        }

        std::string Base64EncodeBytes(const std::vector<unsigned char>& data) {
            static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((data.size() + 2) / 3) * 4);
            int val = 0;
            int valb = -6;
            for (unsigned char c : data) {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0) {
                    out.push_back(table[(val >> valb) & 0x3F]);
                    valb -= 6;
                }
            }
            if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
            while (out.size() % 4) out.push_back('=');
            return out;
        }

        std::vector<unsigned char> Base64DecodeBytes(const std::string& input) {
            static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<int> T(256, -1);
            for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(table[i])] = i;
            std::vector<unsigned char> out;
            int val = 0;
            int valb = -8;
            for (unsigned char c : input) {
                if (c == '=') break;
                if (T[c] == -1) continue;
                val = (val << 6) + T[c];
                valb += 6;
                if (valb >= 0) {
                    out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return out;
        }


        std::wstring Utf8ToWide(const std::string& value) {
#ifdef _WIN32
            if (value.empty()) return {};
            int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
            return out;
#else
            (void)value; return {};
#endif
        }

        std::string WideToUtf8(const wchar_t* value, int length) {
#ifdef _WIN32
            if (!value || length <= 0) return {};
            int needed = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
            if (needed <= 0) return {};
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr);
            return out;
#else
            (void)value; (void)length; return {};
#endif
        }

        std::string ReadLocalClipboardText() {
#ifdef _WIN32
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (OpenClipboard(nullptr)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
                        if (w) {
                            std::string out = WideToUtf8(w, static_cast<int>(wcslen(w)));
                            GlobalUnlock(h); CloseClipboard(); return out;
                        }
                    }
                    h = GetClipboardData(CF_TEXT);
                    if (h) {
                        const char* a = static_cast<const char*>(GlobalLock(h));
                        if (a) { std::string out(a); GlobalUnlock(h); CloseClipboard(); return out; }
                    }
                    CloseClipboard(); return {};
                }
                Sleep(10);
            }
#endif
            return {};
        }

        bool WriteLocalClipboardText(const std::string& text) {
#ifdef _WIN32
            const std::wstring wide = Utf8ToWide(text);
            if (wide.empty() && !text.empty()) return false;
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
                    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (!h) { CloseClipboard(); return false; }
                    void* dst = GlobalLock(h);
                    if (!dst) { GlobalFree(h); CloseClipboard(); return false; }
                    std::memcpy(dst, wide.c_str(), bytes);
                    GlobalUnlock(h);
                    if (!SetClipboardData(CF_UNICODETEXT, h)) { GlobalFree(h); CloseClipboard(); return false; }
                    CloseClipboard(); return true;
                }
                Sleep(10);
            }
#endif
            return false;
        }

        std::filesystem::path DefaultLocalPath() {
#ifdef _WIN32
            char userProfile[MAX_PATH]{};
            DWORD n = GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH);
            if (n > 0 && n < MAX_PATH) return std::filesystem::path(userProfile) / "Desktop";
#endif
            return std::filesystem::current_path();
        }

        std::string ListLocalFilesJson(const std::string& rawPath) {
            namespace fs = std::filesystem;
            fs::path target = rawPath.empty() ? DefaultLocalPath() : fs::path(rawPath);
            std::error_code ec;
            if (!fs::exists(target, ec) || !fs::is_directory(target, ec)) {
                target = DefaultLocalPath();
            }
            std::ostringstream out;
            out << "{\"type\":\"local_file_list\",\"path\":\"" << JsonEscape(target.string()) << "\",\"entries\":[";
            bool first = true;
            fs::path parent = target.parent_path();
            if (!parent.empty() && parent != target) {
                out << "{\"name\":\"..\",\"path\":\"" << JsonEscape(parent.string()) << "\",\"is_dir\":true,\"isDir\":true,\"size\":0}";
                first = false;
            }
            for (const auto& de : fs::directory_iterator(target, fs::directory_options::skip_permission_denied, ec)) {
                if (!first) out << ",";
                first = false;
                bool isDir = de.is_directory(ec);
                std::uintmax_t size = 0;
                if (!isDir) size = de.file_size(ec);
                out << "{\"name\":\"" << JsonEscape(de.path().filename().string())
                    << "\",\"path\":\"" << JsonEscape(de.path().string())
                    << "\",\"is_dir\":" << (isDir ? "true" : "false")
                    << ",\"isDir\":" << (isDir ? "true" : "false")
                    << ",\"size\":" << static_cast<unsigned long long>(size) << "}";
            }
            out << "]}";
            return out.str();
        }

        std::string ReadLocalFileBase64Json(const std::string& path) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path p(path);
            if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
                return "{\"type\":\"local_file_error\",\"error\":\"File does not exist\"}";
            }
            constexpr std::uintmax_t maxInlineBytes = 10ull * 1024ull * 1024ull;
            auto size = fs::file_size(p, ec);
            if (size > maxInlineBytes) {
                return "{\"type\":\"local_file_error\",\"error\":\"File is too large for inline transfer in this build\"}";
            }
            std::ifstream in(p, std::ios::binary);
            if (!in) return "{\"type\":\"local_file_error\",\"error\":\"Could not open local file\"}";
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            std::ostringstream out;
            out << "{\"type\":\"local_file_read\",\"path\":\"" << JsonEscape(p.string())
                << "\",\"name\":\"" << JsonEscape(p.filename().string()) << "\",\"size\":" << bytes.size()
                << ",\"encoding\":\"base64\",\"data\":\"" << Base64EncodeBytes(bytes) << "\"}";
            return out.str();
        }


        std::string ReadLocalTreeBase64Json(const std::string& rootPath) {
            namespace fs = std::filesystem;
            constexpr std::uintmax_t maxTotalBytes = 25ull * 1024ull * 1024ull;
            constexpr int maxItems = 512;
            std::error_code ec;
            fs::path root(rootPath);
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
                return "{\"type\":\"local_file_tree_error\",\"error\":\"Folder does not exist\"}";
            }

            std::ostringstream out;
            out << "{\"type\":\"local_file_tree\",\"path\":\"" << JsonEscape(root.string()) << "\",\"name\":\""
                << JsonEscape(root.filename().string().empty() ? root.string() : root.filename().string()) << "\",\"items\":[";

            bool first = true;
            std::uintmax_t totalBytes = 0;
            int count = 0;

            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
                if (count++ >= maxItems) {
                    return "{\"type\":\"local_file_tree_error\",\"error\":\"Folder contains too many items for inline transfer in this build\"}";
                }

                const fs::path p = it->path();
                const fs::path rel = fs::relative(p, root, ec);
                const bool isDir = it->is_directory(ec);
                if (!first) out << ",";
                first = false;

                out << "{\"name\":\"" << JsonEscape(p.filename().string())
                    << "\",\"path\":\"" << JsonEscape(p.string())
                    << "\",\"rel_path\":\"" << JsonEscape(rel.string())
                    << "\",\"is_dir\":" << (isDir ? "true" : "false")
                    << ",\"isDir\":" << (isDir ? "true" : "false")
                    << ",\"size\":";

                if (isDir) {
                    out << "0}";
                    continue;
                }

                auto size = fs::file_size(p, ec);
                if (ec) size = 0;
                totalBytes += size;
                if (totalBytes > maxTotalBytes) {
                    return "{\"type\":\"local_file_tree_error\",\"error\":\"Folder is too large for inline transfer in this build\"}";
                }

                std::ifstream in(p, std::ios::binary);
                std::vector<unsigned char> bytes(static_cast<size_t>(size));
                if (in && !bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out << static_cast<unsigned long long>(bytes.size())
                    << ",\"encoding\":\"base64\",\"data\":\"" << Base64EncodeBytes(bytes) << "\"}";
            }
            out << "]}";
            return out.str();
        }

        std::string WriteLocalFileBase64Json(const std::string& reqJson) {
            const std::string path = ExtractJsonStringField(reqJson, "path");
            const std::string data = ExtractJsonStringField(reqJson, "data");
            if (path.empty()) return "{\"type\":\"local_file_write_error\",\"error\":\"Missing target path\"}";
            try {
                std::filesystem::path p(path);
                std::filesystem::create_directories(p.parent_path());
                auto bytes = Base64DecodeBytes(data);
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                if (!out) return "{\"type\":\"local_file_write_error\",\"error\":\"Could not open target file\"}";
                if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                return std::string("{\"type\":\"local_file_write_complete\",\"path\":\"") + JsonEscape(p.string()) + "\",\"size\":" + std::to_string(bytes.size()) + "}";
            }
            catch (const std::exception& e) {
                return std::string("{\"type\":\"local_file_write_error\",\"error\":\"") + JsonEscape(e.what()) + "\"}";
            }
        }

        std::string WriteLocalFileChunkJson(const std::string& reqJson) {
            const std::string path = ExtractJsonStringField(reqJson, "path");
            const std::string data = ExtractJsonStringField(reqJson, "data");
            const std::string mode = ExtractJsonStringField(reqJson, "mode");
            if (path.empty()) return "{\"type\":\"local_file_chunk_error\",\"error\":\"Missing target path\"}";
            try {
                std::filesystem::path p(path);
                std::filesystem::create_directories(p.parent_path());
                auto bytes = Base64DecodeBytes(data);
                std::ios::openmode flags = std::ios::binary;
                if (mode == "start") flags |= std::ios::trunc;
                else flags |= std::ios::app;
                std::ofstream out(p, flags);
                if (!out) return "{\"type\":\"local_file_chunk_error\",\"error\":\"Could not open target file\"}";
                if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                return std::string("{\"type\":\"local_file_chunk_complete\",\"path\":\"") + JsonEscape(p.string()) + "\",\"size\":" + std::to_string(bytes.size()) + "}";
            }
            catch (const std::exception& e) {
                return std::string("{\"type\":\"local_file_chunk_error\",\"error\":\"") + JsonEscape(e.what()) + "\"}";
            }
        }

        std::string LocalDeleteJson(const std::string& path) {
            try {
                std::filesystem::path p(path);
                std::uintmax_t removed = std::filesystem::is_directory(p) ? std::filesystem::remove_all(p) : (std::filesystem::remove(p) ? 1 : 0);
                return std::string("{\"type\":\"local_file_delete_complete\",\"path\":\"") + JsonEscape(path) + "\",\"removed\":" + std::to_string(removed) + "}";
            }
            catch (const std::exception& e) {
                return std::string("{\"type\":\"local_file_error\",\"error\":\"") + JsonEscape(e.what()) + "\"}";
            }
        }

        std::string LocalMkdirJson(const std::string& path) {
            try {
                std::filesystem::create_directories(std::filesystem::path(path));
                return std::string("{\"type\":\"local_file_mkdir_complete\",\"path\":\"") + JsonEscape(path) + "\"}";
            }
            catch (const std::exception& e) {
                return std::string("{\"type\":\"local_file_error\",\"error\":\"") + JsonEscape(e.what()) + "\"}";
            }
        }

        std::string LocalRenameJson(const std::string& reqJson) {
            const std::string from = ExtractJsonStringField(reqJson, "from");
            const std::string to = ExtractJsonStringField(reqJson, "to");
            try {
                std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to));
                return std::string("{\"type\":\"local_file_rename_complete\",\"from\":\"") + JsonEscape(from) + "\",\"to\":\"" + JsonEscape(to) + "\"}";
            }
            catch (const std::exception& e) {
                return std::string("{\"type\":\"local_file_error\",\"error\":\"") + JsonEscape(e.what()) + "\"}";
            }
        }

        std::string BuildChatWindowHtml() {
            return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Hi5Central Support Chat</title>
<style>
*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;overflow:hidden}
:root{--bg:#f7f9fc;--surface:#ffffff;--surface2:#f8fafc;--border:#dbe3ec;--text:#111827;--muted:#64748b;--accent:#2563eb;--accent-soft:#eff6ff;--success:#22c55e}
body{font-family:"Segoe UI Variable","Segoe UI",Arial,sans-serif;background:var(--bg);color:var(--text)}
.shell{height:100dvh;display:flex;flex-direction:column;background:var(--bg)}
.head{min-height:68px;display:flex;align-items:center;gap:11px;padding:12px 14px;border-bottom:1px solid var(--border);background:rgba(248,250,252,.97)}
.badge{width:36px;height:36px;flex:0 0 auto;border-radius:10px;background:var(--accent);color:#fff;display:flex;align-items:center;justify-content:center;font-size:11px;font-weight:900;box-shadow:0 8px 20px rgba(37,99,235,.22)}
.titles{min-width:0;flex:1}.title{font-weight:750;font-size:15px;letter-spacing:-.15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.sub{font-size:11px;color:var(--muted);margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.x{border:1px solid var(--border);background:var(--surface);color:#475569;border-radius:9px;width:34px;height:34px;font-size:15px;cursor:pointer}.x:hover{background:#fff5f5;border-color:#fecaca;color:#dc2626}
.messages{flex:1;min-height:0;overflow:auto;padding:14px 13px 10px;scroll-behavior:smooth}.empty{height:100%;display:flex;align-items:center;justify-content:center;text-align:center;color:var(--muted);font-size:12px;line-height:1.5;padding:20px}
.row{display:flex;flex-direction:column;margin:0 0 11px;max-width:88%}.row.tech{margin-left:auto;align-items:flex-end}.row.user{margin-right:auto;align-items:flex-start}
.meta{font-size:10px;color:var(--muted);margin:0 7px 4px}.bubble{border-radius:14px;padding:9px 11px;font-size:13px;line-height:1.42;white-space:pre-wrap;word-break:break-word;box-shadow:0 6px 18px rgba(15,23,42,.08)}
.tech .bubble{background:var(--accent);color:#fff;border-bottom-right-radius:5px}.user .bubble{background:var(--surface);color:var(--text);border:1px solid var(--border);border-bottom-left-radius:5px}
.compose{border-top:1px solid var(--border);background:var(--surface2);padding:10px;display:flex;gap:8px;align-items:flex-end}
textarea{flex:1;min-width:0;min-height:40px;max-height:104px;resize:none;border:1px solid var(--border);background:var(--surface);color:var(--text);border-radius:11px;padding:10px 11px;font:13px/1.35 inherit;outline:none}
textarea:focus{border-color:#93b9ff;box-shadow:0 0 0 3px rgba(37,99,235,.09)}
.send{border:1px solid var(--accent);border-radius:10px;background:var(--accent);color:#fff;min-width:68px;height:40px;font-weight:750;cursor:pointer}.send:hover{background:#1d4ed8}.send:disabled{opacity:.45;cursor:default}
@media(max-width:360px){.head{padding:9px 10px;min-height:58px}.badge{width:32px;height:32px}.title{font-size:14px}.sub{display:none}.messages{padding:10px 8px}.compose{padding:8px;gap:6px}.send{min-width:58px}.row{max-width:94%}}
</style>
</head>
<body>
<div class="shell">
  <div class="head"><div class="badge">H5</div><div class="titles"><div class="title">Hi5Central Support Chat</div><div class="sub">Technician chat</div></div><button class="x" id="closeBtn" title="Close chat">X</button></div>
  <div class="messages" id="messages"><div class="empty" id="empty">No messages yet. Send a message to start a support chat with the remote user.</div></div>
  <div class="compose"><textarea id="input" placeholder="Type a message..."></textarea><button class="send" id="send">Send</button></div>
</div>
<script>
(function(){
  const messages=document.getElementById('messages'),empty=document.getElementById('empty'),input=document.getElementById('input'),send=document.getElementById('send'),closeBtn=document.getElementById('closeBtn');
  function normSender(s){s=String(s||'').toLowerCase();return(s==='tech'||s==='technician'||s==='viewer'||s==='me')?'tech':'user'}
  function bodyOf(m){return String((m&&(m.body??m.message??m.text??m.content))||'').trim()}
  function add(m){const body=bodyOf(m);if(!body)return;if(empty)empty.style.display='none';const sender=normSender(m.sender);const row=document.createElement('div');row.className='row '+sender;const meta=document.createElement('div');meta.className='meta';meta.textContent=m.display_name||m.displayName||(sender==='tech'?'Technician':'Remote user');const bubble=document.createElement('div');bubble.className='bubble';bubble.textContent=body;row.appendChild(meta);row.appendChild(bubble);messages.appendChild(row);messages.scrollTop=messages.scrollHeight}
  window.__hi5ChatReceive=function(m){try{if(typeof m==='string')m=JSON.parse(m);add(m)}catch(e){console.error(e)}};
  async function doSend(){const body=input.value.trim();if(!body)return;input.value='';try{if(window.hi5ChatSend)await window.hi5ChatSend(body);}catch(e){console.error(e)}}
  send.addEventListener('click',doSend);
  input.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();doSend()}});
  closeBtn.addEventListener('click',()=>{try{if(window.hi5ChatClose)window.hi5ChatClose('');else window.close()}catch(e){}});
  setTimeout(()=>input.focus(),100);
  try{if(window.hi5ChatReady)window.hi5ChatReady('')}catch(e){}
})();
</script>
</body>
</html>)HTML";
        }


        std::string BuildFileBrowserMissingHtml() {
            return "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
                "html,body{margin:0;height:100%;background:#0b1020;color:#eef2ff;font-family:Segoe UI,Arial,sans-serif;display:flex;align-items:center;justify-content:center}"
                ".card{max-width:620px;border:1px solid rgba(255,255,255,.12);border-radius:18px;background:#111827;padding:24px;box-shadow:0 20px 60px rgba(0,0,0,.35)}"
                ".title{font-weight:800;font-size:18px;margin-bottom:8px}.sub{opacity:.7;line-height:1.5;white-space:pre-wrap}"
                "</style></head><body><div class=\"card\"><div class=\"title\">File browser UI missing</div>"
                "<div class=\"sub\">Could not load web/file_browser.html. Rebuild after replacing CMakeLists.txt so the file is copied into the build output.</div></div></body></html>";
        }


        struct ChatBridge {
            std::mutex mutex;
            webview::webview* mainWindow = nullptr;
            webview::webview* chatWindow = nullptr;
            bool chatRunning = false;
            bool chatReady = false;
            std::vector<std::string> queuedMessages;
        };

#ifdef _WIN32
        HWND GetWebviewHwnd(webview::webview& w) {
            auto result = w.window();
            if (!result.ok()) return nullptr;
            return static_cast<HWND>(result.value());
        }

        HHOOK gKeyboardHook = nullptr;
        HWND gMainViewerHwnd = nullptr;
        webview::webview* gMainViewerWebview = nullptr;
        DWORD gMainViewerPid = 0;
        bool gWinKeyDown = false;
        bool gWinComboUsed = false;
        bool gRemoteAltTabActive = false;

        bool IsViewerForegroundAndMaximized() {
            if (!gMainViewerHwnd || !IsWindow(gMainViewerHwnd)) return false;
            if (!IsZoomed(gMainViewerHwnd)) return false;

            HWND fg = GetForegroundWindow();
            if (!fg) return false;

            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != gMainViewerPid) return false;

            return fg == gMainViewerHwnd || IsChild(gMainViewerHwnd, fg);
        }

        void DispatchNativeViewerShortcut(const std::string& action) {
            auto* main = gMainViewerWebview;
            if (!main) return;
            main->dispatch([main, action]() {
                main->eval("window.__hi5NativeShortcut && window.__hi5NativeShortcut(\"" + JsEscape(action) + "\");");
                });
        }

        const char* NativeWinShortcutForVk(DWORD vk) {
            switch (vk) {
            case 'D': return "win_d";
            case 'R': return "win_r";
            case 'E': return "win_e";
            case 'L': return "lock";
            case VK_TAB: return "win_tab";
            case VK_ESCAPE: return "start_menu";
            default: return nullptr;
            }
        }

        LRESULT CALLBACK ViewerKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
            if (code == HC_ACTION && lParam) {
                const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
                const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                const bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
                const bool viewerReady = IsViewerForegroundAndMaximized();

                if (!viewerReady) {
                    if (gRemoteAltTabActive) {
                        DispatchNativeViewerShortcut("alt_tab_end");
                    }
                    gRemoteAltTabActive = false;
                    gWinKeyDown = false;
                    gWinComboUsed = false;
                    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
                }

                const bool isWinKey = (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN);

                // Hold the local Windows key. If it is pressed and released alone,
                // open the remote Start menu on key-up. If it is used with D/R/E/L/Tab,
                // send the matching remote shortcut and swallow all local Windows input.
                if (isWinKey) {
                    if (keyDown) {
                        gWinKeyDown = true;
                        return 1;
                    }
                    if (keyUp) {
                        if (gWinKeyDown && !gWinComboUsed) {
                            DispatchNativeViewerShortcut("start_menu");
                        }
                        gWinKeyDown = false;
                        gWinComboUsed = false;
                        return 1;
                    }
                }

                if (gWinKeyDown) {
                    if (keyDown) {
                        if (const char* action = NativeWinShortcutForVk(k->vkCode)) {
                            DispatchNativeViewerShortcut(action);
                            gWinComboUsed = true;
                        }
                    }
                    // Swallow all key-down/key-up while Win is held so local Windows
                    // shortcuts never leak to the technician machine.
                    return 1;
                }

                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                // Dedicated remote SAS request. Windows will not deliver the real
                // physical Ctrl+Alt+Del to normal apps, but Ctrl+Alt+End is the
                // standard remote-session equivalent. If Delete is delivered in some
                // environments, handle that too.
                if (keyDown && ctrl && alt && (k->vkCode == VK_END || k->vkCode == VK_DELETE)) {
                    DispatchNativeViewerShortcut("ctrl_alt_del_service");
                    return 1;
                }

                const bool isAltKey = (k->vkCode == VK_MENU || k->vkCode == VK_LMENU || k->vkCode == VK_RMENU);

                if (keyDown && alt && k->vkCode == VK_TAB) {
                    DispatchNativeViewerShortcut(gRemoteAltTabActive ? "alt_tab_next" : "alt_tab_begin");
                    gRemoteAltTabActive = true;
                    return 1;
                }
                if (keyUp && k->vkCode == VK_TAB && gRemoteAltTabActive) {
                    return 1;
                }
                if (keyUp && isAltKey && gRemoteAltTabActive) {
                    DispatchNativeViewerShortcut("alt_tab_end");
                    gRemoteAltTabActive = false;
                    return 1;
                }

                if (keyDown && alt && k->vkCode == VK_F4) {
                    DispatchNativeViewerShortcut("alt_f4");
                    return 1;
                }
                if (keyUp && alt && k->vkCode == VK_F4) {
                    return 1;
                }

                if (keyDown && ctrl && shift && k->vkCode == VK_ESCAPE) {
                    DispatchNativeViewerShortcut("ctrl_shift_esc");
                    return 1;
                }
                if (keyDown && ctrl && !shift && k->vkCode == VK_ESCAPE) {
                    DispatchNativeViewerShortcut("ctrl_esc");
                    return 1;
                }
            }
            return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
        }

        void InstallViewerKeyboardHook(webview::webview& w) {
            gMainViewerWebview = &w;
            gMainViewerHwnd = GetWebviewHwnd(w);
            gMainViewerPid = GetCurrentProcessId();
            if (!gKeyboardHook) {
                gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, ViewerKeyboardHookProc, GetModuleHandleW(nullptr), 0);
                if (gKeyboardHook) {
                    LogInfo("[viewer-input] native keyboard hook installed for Windows/Alt/Ctrl shortcut forwarding");
                }
                else {
                    LogWarn("[viewer-input] failed to install keyboard hook err=" + std::to_string(GetLastError()));
                }
            }
        }

        void UninstallViewerKeyboardHook() {
            if (gKeyboardHook) {
                UnhookWindowsHookEx(gKeyboardHook);
                gKeyboardHook = nullptr;
            }
            gMainViewerWebview = nullptr;
            gMainViewerHwnd = nullptr;
            gMainViewerPid = 0;
            if (gRemoteAltTabActive) {
                DispatchNativeViewerShortcut("alt_tab_end");
            }
            gRemoteAltTabActive = false;
            gWinKeyDown = false;
            gWinComboUsed = false;
        }
#endif


        struct FileBridge {
            std::mutex mutex;
            webview::webview* mainWindow = nullptr;
            webview::webview* fileWindow = nullptr;
            bool fileRunning = false;
            bool fileReady = false;
            std::string fileHtml;
            std::string localLabel = "This PC";
            std::string remoteLabel = "Remote device";
            std::vector<std::string> queuedMessages;
#ifdef _WIN32
            bool fileMaximized = false;
            RECT fileRestoreRect{};
#endif
        };

        void DispatchFileToMain(const std::shared_ptr<FileBridge>& bridge, const std::string& js) {
            webview::webview* main = nullptr;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                main = bridge->mainWindow;
            }
            if (main) main->dispatch([main, js]() { main->eval(js); });
        }

        void PostMessageToFileWindow(const std::shared_ptr<FileBridge>& bridge, const std::string& msgJson) {
            std::lock_guard<std::mutex> lock(bridge->mutex);
            if (!bridge->fileWindow || !bridge->fileReady) {
                bridge->queuedMessages.push_back(msgJson);
                return;
            }
            auto* file = bridge->fileWindow;
            file->dispatch([file, msgJson]() { file->eval("window.__hi5FilesReceive(" + msgJson + ");"); });
        }

        void FlushFileQueue(const std::shared_ptr<FileBridge>& bridge) {
            std::vector<std::string> queued;
            webview::webview* file = nullptr;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                if (!bridge->fileWindow || !bridge->fileReady) return;
                file = bridge->fileWindow;
                queued.swap(bridge->queuedMessages);
            }
            for (const auto& msgJson : queued) {
                file->dispatch([file, msgJson]() { file->eval("window.__hi5FilesReceive(" + msgJson + ");"); });
            }
        }

        void OpenFileWindow(const std::shared_ptr<FileBridge>& bridge) {
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                if (bridge->fileRunning) {
#ifdef _WIN32
                    if (bridge->fileWindow) {
                        HWND hwnd = GetWebviewHwnd(*bridge->fileWindow);
                        if (hwnd) { ShowWindow(hwnd, SW_SHOWNORMAL); SetForegroundWindow(hwnd); }
                    }
#endif
                    return;
                }
                bridge->fileRunning = true;
                bridge->fileReady = false;
            }
            std::thread([bridge]() {
#ifdef _WIN32
                HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); (void)co;
#endif
                try {
                    LogInfo("[viewer-files] creating native file browser WebView window");
                    webview::webview file(true, nullptr);
                    file.set_title("Hi5Central File Browser");
                    file.set_size(1120, 740, WEBVIEW_HINT_NONE);

                    file.bind("hi5FileReady", [bridge](std::string) -> std::string { { std::lock_guard<std::mutex> lock(bridge->mutex); bridge->fileReady = true; } FlushFileQueue(bridge); return "true"; });
                    file.bind("hi5FileClose", [&file](std::string) -> std::string { file.terminate(); return "true"; });
                    file.bind("hi5FileGetContext", [bridge](std::string) -> std::string {
                        std::lock_guard<std::mutex> lock(bridge->mutex);
                        return std::string("{\"local_label\":\"") + JsonEscape(bridge->localLabel) + "\",\"remote_label\":\"" + JsonEscape(bridge->remoteLabel) + "\"}";
                        });
#ifdef _WIN32
                    file.bind("hi5FileMinimize", [&file](std::string) -> std::string { HWND hwnd = GetWebviewHwnd(file); if (hwnd) ShowWindow(hwnd, SW_MINIMIZE); return "true"; });
                    file.bind("hi5FileToggleMaximize", [&file](std::string) -> std::string {
                        HWND hwnd = GetWebviewHwnd(file);
                        if (!hwnd) return "false";

                        // Use Windows' own maximise/restore behaviour rather than manual full-screen sizing.
                        // This preserves the taskbar/work-area behaviour and works better with Snap Assist.
                        if (IsZoomed(hwnd)) {
                            ShowWindow(hwnd, SW_RESTORE);
                        }
                        else {
                            ShowWindow(hwnd, SW_MAXIMIZE);
                        }
                        return "true";
                        });
                    file.bind("hi5FileBeginDrag", [&file](std::string) -> std::string {
                        HWND hwnd = GetWebviewHwnd(file);
                        if (!hwnd) return "false";
                        if (IsZoomed(hwnd)) return "true";
                        ReleaseCapture();
                        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                        return "true";
                        });
                    file.bind("hi5FileMoveWindow", [&file](std::string req) -> std::string {
                        HWND hwnd = GetWebviewHwnd(file);
                        if (!hwnd || IsZoomed(hwnd)) return "false";

                        int dx = 0;
                        int dy = 0;
                        try {
                            auto getInt = [&](const std::string& key) -> int {
                                const std::string marker = "\"" + key + "\"";
                                size_t pos = req.find(marker);
                                if (pos == std::string::npos) return 0;
                                pos = req.find(':', pos);
                                if (pos == std::string::npos) return 0;
                                ++pos;
                                while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
                                size_t end = pos;
                                if (end < req.size() && (req[end] == '-' || req[end] == '+')) ++end;
                                while (end < req.size() && req[end] >= '0' && req[end] <= '9') ++end;
                                return std::stoi(req.substr(pos, end - pos));
                                };
                            dx = getInt("dx");
                            dy = getInt("dy");
                        }
                        catch (...) {
                            return "false";
                        }

                        if (dx == 0 && dy == 0) return "true";
                        RECT r{};
                        if (!GetWindowRect(hwnd, &r)) return "false";
                        SetWindowPos(hwnd, nullptr, r.left + dx, r.top + dy, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        return "true";
                        });
#endif
                    file.bind("hi5FileLocalList", [](std::string req) -> std::string { return ListLocalFilesJson(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalRead", [](std::string req) -> std::string { return ReadLocalFileBase64Json(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalReadTree", [](std::string req) -> std::string { return ReadLocalTreeBase64Json(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalWrite", [](std::string req) -> std::string { return WriteLocalFileBase64Json(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalWriteChunk", [](std::string req) -> std::string { return WriteLocalFileChunkJson(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalDelete", [](std::string req) -> std::string { return LocalDeleteJson(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalMkdir", [](std::string req) -> std::string { return LocalMkdirJson(ParseFirstJsonStringArg(req)); });
                    file.bind("hi5FileLocalRename", [](std::string req) -> std::string { return LocalRenameJson(ParseFirstJsonStringArg(req)); });

                    file.bind("hi5FileRemoteList", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesRequestRemoteList(\"" + JsEscape(ParseFirstJsonStringArg(req)) + "\");"); return "true"; });
                    file.bind("hi5FileRemoteUpload", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesUploadRemote(" + ParseFirstJsonStringArg(req) + ");"); return "true"; });
                    file.bind("hi5FileRemoteDownload", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesDownloadRemote(" + ParseFirstJsonStringArg(req) + ");"); return "true"; });
                    file.bind("hi5FileRemoteDelete", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesDeleteRemote(\"" + JsEscape(ParseFirstJsonStringArg(req)) + "\");"); return "true"; });
                    file.bind("hi5FileRemoteMkdir", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesMkdirRemote(\"" + JsEscape(ParseFirstJsonStringArg(req)) + "\");"); return "true"; });
                    file.bind("hi5FileRemoteRename", [bridge](std::string req) -> std::string { DispatchFileToMain(bridge, "window.__hi5FilesRenameRemote(" + ParseFirstJsonStringArg(req) + ");"); return "true"; });

                    { std::lock_guard<std::mutex> lock(bridge->mutex); bridge->fileWindow = &file; }
                    std::string fileHtml;
                    {
                        std::lock_guard<std::mutex> lock(bridge->mutex);
                        fileHtml = bridge->fileHtml;
                    }
                    file.set_html(fileHtml.empty() ? BuildFileBrowserMissingHtml() : fileHtml);
#ifdef _WIN32
                    HWND hwnd = GetWebviewHwnd(file);
                    if (hwnd) {
                        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                        const int workWidth = static_cast<int>(work.right - work.left);
                        const int workHeight = static_cast<int>(work.bottom - work.top);
                        const int availWidth = std::max<int>(280, workWidth - 32);
                        const int availHeight = std::max<int>(260, workHeight - 32);
                        const int width = std::min<int>(1120, availWidth);
                        const int height = std::min<int>(740, availHeight);
                        const int minWidth = std::min<int>(520, availWidth);
                        const int minHeight = std::min<int>(420, availHeight);
                        file.set_size(minWidth, minHeight, WEBVIEW_HINT_MIN);
                        file.set_size(workWidth, workHeight, WEBVIEW_HINT_MAX);
                        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                        // Use the normal Windows title bar/minimise/maximise/close controls.
                        // This preserves Windows Snap Assist and avoids crashes caused by custom maximise/minimise handling.
                        style |= WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
                        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
                        const int x = static_cast<int>(work.left) + std::max<int>(20, (workWidth - width) / 2);
                        const int y = static_cast<int>(work.top) + std::max<int>(20, (workHeight - height) / 2);
                        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                        ShowWindow(hwnd, SW_SHOWNORMAL);
                        SetForegroundWindow(hwnd);
                    }
#endif
                    LogInfo("[viewer-files] native file browser window shown");
                    file.run();
                }
                catch (const std::exception& e) { LogError(std::string("[viewer-files] file browser failed: ") + e.what()); }
                catch (...) { LogError("[viewer-files] file browser failed with unknown exception"); }
                {
                    std::lock_guard<std::mutex> lock(bridge->mutex);
                    bridge->fileWindow = nullptr;
                    bridge->fileRunning = false;
                    bridge->fileReady = false;
                    bridge->queuedMessages.clear();
#ifdef _WIN32
                    bridge->fileMaximized = false;
                    bridge->fileRestoreRect = RECT{};
#endif
                }
#ifdef _WIN32
                CoUninitialize();
#endif
                }).detach();
        }

        void CloseFileWindow(const std::shared_ptr<FileBridge>& bridge) {
            webview::webview* file = nullptr;
            { std::lock_guard<std::mutex> lock(bridge->mutex); file = bridge->fileWindow; }
            if (file) file->dispatch([file]() { file->terminate(); });
        }

        void DispatchToMain(const std::shared_ptr<ChatBridge>& bridge, const std::string& js) {
            webview::webview* main = nullptr;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                main = bridge->mainWindow;
            }
            if (main) {
                main->dispatch([main, js]() { main->eval(js); });
            }
        }

        void PostMessageToChatWindow(const std::shared_ptr<ChatBridge>& bridge, const std::string& msgJson) {
            std::lock_guard<std::mutex> lock(bridge->mutex);
            if (!bridge->chatWindow || !bridge->chatReady) {
                bridge->queuedMessages.push_back(msgJson);
                return;
            }
            auto* chat = bridge->chatWindow;
            chat->dispatch([chat, msgJson]() {
                chat->eval("window.__hi5ChatReceive(" + msgJson + ");");
                });
        }

        void FlushChatQueue(const std::shared_ptr<ChatBridge>& bridge) {
            std::vector<std::string> queued;
            webview::webview* chat = nullptr;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                if (!bridge->chatWindow || !bridge->chatReady) return;
                chat = bridge->chatWindow;
                queued.swap(bridge->queuedMessages);
            }
            for (const auto& msgJson : queued) {
                chat->dispatch([chat, msgJson]() {
                    chat->eval("window.__hi5ChatReceive(" + msgJson + ");");
                    });
            }
        }

        void OpenChatWindow(const std::shared_ptr<ChatBridge>& bridge) {
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                if (bridge->chatRunning) {
#ifdef _WIN32
                    if (bridge->chatWindow) {
                        HWND hwnd = GetWebviewHwnd(*bridge->chatWindow);
                        if (hwnd) {
                            ShowWindow(hwnd, SW_SHOWNORMAL);
                            SetForegroundWindow(hwnd);
                        }
                    }
#endif
                    return;
                }
                bridge->chatRunning = true;
                bridge->chatReady = false;
            }

            std::thread([bridge]() {
#ifdef _WIN32
                HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                (void)co;
#endif
                try {
                    LogInfo("[viewer-chat] creating native chat WebView window");
                    webview::webview chat(true, nullptr);
                    chat.set_title("Hi5Central Support Chat");
                    chat.set_size(420, 620, WEBVIEW_HINT_NONE);

                    chat.bind("hi5ChatSend", [bridge](std::string req) -> std::string {
                        const auto body = ParseFirstJsonStringArg(req);
                        if (!body.empty()) {
                            DispatchToMain(bridge, "window.__hi5ChatSendFromNative(\"" + JsEscape(body) + "\");");
                        }
                        return "true";
                        });

                    chat.bind("hi5ChatReady", [bridge](std::string) -> std::string {
                        {
                            std::lock_guard<std::mutex> lock(bridge->mutex);
                            bridge->chatReady = true;
                        }
                        FlushChatQueue(bridge);
                        return "true";
                        });

                    chat.bind("hi5ChatClose", [&chat](std::string) -> std::string {
                        chat.terminate();
                        return "true";
                        });

                    {
                        std::lock_guard<std::mutex> lock(bridge->mutex);
                        bridge->chatWindow = &chat;
                    }

                    chat.set_html(BuildChatWindowHtml());

#ifdef _WIN32
                    HWND hwnd = GetWebviewHwnd(chat);
                    if (hwnd) {
                        RECT work{};
                        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                        const int workWidth = std::max<int>(1, work.right - work.left);
                        const int workHeight = std::max<int>(1, work.bottom - work.top);
                        const int availWidth = std::max<int>(240, workWidth - 32);
                        const int availHeight = std::max<int>(280, workHeight - 32);
                        const int width = std::min<int>(420, availWidth);
                        const int height = std::min<int>(620, availHeight);
                        const int minWidth = std::min<int>(300, availWidth);
                        const int minHeight = std::min<int>(320, availHeight);
                        chat.set_size(minWidth, minHeight, WEBVIEW_HINT_MIN);
                        chat.set_size(workWidth, workHeight, WEBVIEW_HINT_MAX);
                        const int x = std::max<int>(work.left, work.right - width - 24);
                        const int y = std::max<int>(work.top, work.bottom - height - 24);
                        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                        ShowWindow(hwnd, SW_SHOWNORMAL);
                        SetForegroundWindow(hwnd);
                    }
#endif

                    LogInfo("[viewer-chat] native chat window shown");
                    chat.run();
                }
                catch (const std::exception& e) {
                    LogError(std::string("[viewer-chat] native chat window failed: ") + e.what());
                }
                catch (...) {
                    LogError("[viewer-chat] native chat window failed with unknown exception");
                }

                {
                    std::lock_guard<std::mutex> lock(bridge->mutex);
                    bridge->chatWindow = nullptr;
                    bridge->chatRunning = false;
                    bridge->chatReady = false;
                    bridge->queuedMessages.clear();
                }
#ifdef _WIN32
                CoUninitialize();
#endif
                }).detach();
        }

        void CloseChatWindow(const std::shared_ptr<ChatBridge>& bridge) {
            webview::webview* chat = nullptr;
            {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                chat = bridge->chatWindow;
            }
            if (chat) {
                chat->dispatch([chat]() { chat->terminate(); });
            }
        }

        std::string BuildBootstrapScript(const DeepLinkLaunch& launch) {
            std::ostringstream js;
            js
                << "<script>\n"
                << "(function(){\n"
                << "  const pending = {\n"
                << "    session_id: \"" << JsEscape(launch.sessionId) << "\",\n"
                << "    sessionId: \"" << JsEscape(launch.sessionId) << "\",\n"
                << "    token: \"" << JsEscape(launch.token) << "\",\n"
                << "    device_id: \"" << JsEscape(launch.deviceId) << "\",\n"
                << "    deviceId: \"" << JsEscape(launch.deviceId) << "\",\n"
                << "    wss_url: \"" << JsEscape(launch.wssUrl) << "\",\n"
                << "    wssUrl: \"" << JsEscape(launch.wssUrl) << "\"\n"
                << "  };\n"
                << "\n"
                << "  let connectHandler = null;\n"
                << "\n"
                << "  window.hi5 = window.hi5 || {};\n"
                << "\n"
                << "  window.hi5.onConnect = function(cb) {\n"
                << "    connectHandler = cb;\n"
                << "    console.log('[native-host] onConnect registered');\n"
                << "    if (typeof cb === 'function' && pending.sessionId && pending.deviceId && pending.wssUrl) {\n"
                << "      try {\n"
                << "        console.log('[native-host] delivering pending connect payload', pending);\n"
                << "        cb(pending);\n"
                << "      } catch (e) {\n"
                << "        console.error('[native-host] onConnect callback failed', e);\n"
                << "      }\n"
                << "    }\n"
                << "    return function() {};\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.getPendingConnect = async function() {\n"
                << "    return (pending.sessionId && pending.deviceId && pending.wssUrl) ? pending : null;\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.signalRendererReady = function() {\n"
                << "    console.log('[native-host] renderer ready');\n"
                << "    if (connectHandler && pending.sessionId && pending.deviceId && pending.wssUrl) {\n"
                << "      try {\n"
                << "        connectHandler(pending);\n"
                << "      } catch (e) {\n"
                << "        console.error('[native-host] renderer-ready callback failed', e);\n"
                << "      }\n"
                << "    }\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.notifyConnected = function(deviceId) {\n"
                << "    console.log('[native-host] notifyConnected', deviceId || '');\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.notifyDisconnected = function() {\n"
                << "    console.log('[native-host] notifyDisconnected');\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.openChatWindow = function() {\n"
                << "    if (typeof window.hi5OpenChatWindow === 'function') return window.hi5OpenChatWindow('');\n"
                << "    return false;\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.postChatMessageToWindow = function(json) {\n"
                << "    if (typeof window.hi5PostChatMessageToWindow === 'function') return window.hi5PostChatMessageToWindow(String(json || ''));\n"
                << "    return false;\n"
                << "  };\n"
                << "\n"
                << "  window.hi5.closeChatWindow = function() {\n"
                << "    if (typeof window.hi5CloseChatWindow === 'function') return window.hi5CloseChatWindow('');\n"
                << "    return false;\n"
                << "  };\n"
                << "\n"
                << "  window.__HI5_PENDING_CONNECT__ = pending;\n"
                << "})();\n"
                << "</script>\n";
            return js.str();
        }

        std::string BuildInlineScriptTag(const std::string& jsSource) {
            std::ostringstream out;
            out << "<script>\n" << jsSource << "\n</script>\n";
            return out.str();
        }

        std::string InjectScriptsIntoHtml(
            const std::string& html,
            const std::string& bootstrapScriptTag,
            const std::string& rendererInlineScriptTag) {

            const std::string externalRendererTag = "<script src=\"renderer.js\"></script>";
            const auto rendererPos = html.find(externalRendererTag);

            if (rendererPos != std::string::npos) {
                std::string out = html;
                out.replace(
                    rendererPos,
                    externalRendererTag.size(),
                    bootstrapScriptTag + rendererInlineScriptTag
                );
                return out;
            }

            const auto bodyPos = html.find("</body>");
            if (bodyPos != std::string::npos) {
                std::string out = html;
                out.insert(bodyPos, bootstrapScriptTag + rendererInlineScriptTag);
                return out;
            }

            return html + bootstrapScriptTag + rendererInlineScriptTag;
        }

        std::string BuildErrorHtml(const std::string& message) {
            std::ostringstream html;
            html
                << "<!doctype html><html><head><meta charset=\"utf-8\">"
                << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                << "<title>Hi5Central Viewer</title>"
                << "<style>"
                << "html,body{margin:0;padding:0;width:100%;height:100%;background:#0f1117;color:#e6edf3;"
                << "font-family:Segoe UI,Arial,sans-serif;display:flex;align-items:center;justify-content:center;}"
                << ".card{max-width:720px;margin:24px;background:#161b22;border:1px solid rgba(255,255,255,0.08);"
                << "border-radius:16px;padding:24px;box-shadow:0 12px 30px rgba(0,0,0,.28);}"
                << ".title{font-size:22px;font-weight:700;margin-bottom:10px;}"
                << ".msg{font-size:14px;line-height:1.5;opacity:.9;white-space:pre-wrap;}"
                << "</style></head><body>"
                << "<div class=\"card\"><div class=\"title\">Hi5Central Viewer</div>"
                << "<div class=\"msg\">" << message << "</div></div></body></html>";
            return html.str();
        }

    } // namespace

    int ViewerApp::Run(int argc, char* argv[]) {
        LogInfo("Viewer starting");
        LogInfo("argc=" + std::to_string(argc));

        DeepLinkLaunch launch{};
        if (argc >= 2 && argv[1]) {
            launch = ParseDeepLink(argv[1]);
            LogInfo("Deep link provided via argv");
            LogInfo("Deep link parsed successfully");
            LogInfo("Deep link session_id=" + launch.sessionId);
            LogInfo("Deep link device_id=" + launch.deviceId);
            LogInfo("Deep link mode=" + launch.mode);
        }
        else {
            LogWarn("No deep link provided");
        }

        std::filesystem::path exeDir;
#ifdef _WIN32
        char modulePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        exeDir = std::filesystem::path(modulePath).parent_path();
#else
        exeDir = std::filesystem::current_path();
#endif

        const auto indexPath = exeDir / "web" / "index.html";
        const auto rendererPath = exeDir / "web" / "renderer.js";
        const auto fileBrowserPath = exeDir / "web" / "file_browser.html";

        LogInfo("indexPath=" + indexPath.string());
        LogInfo("rendererPath=" + rendererPath.string());
        LogInfo("fileBrowserPath=" + fileBrowserPath.string());

        webview::webview w(true, nullptr);
        auto chatBridge = std::make_shared<ChatBridge>();
        chatBridge->mainWindow = &w;
        auto fileBridge = std::make_shared<FileBridge>();
        fileBridge->mainWindow = &w;
        fileBridge->localLabel = GetLocalComputerLabel();
        fileBridge->remoteLabel = "Remote device";
        fileBridge->fileHtml = ReadTextFileUtf8(fileBrowserPath);
        if (fileBridge->fileHtml.empty()) {
            LogWarn("File browser HTML missing or empty: " + fileBrowserPath.string());
        }

        w.bind("hi5OpenChatWindow", [chatBridge](std::string) -> std::string {
            LogInfo("[viewer-chat] open requested from renderer");
            OpenChatWindow(chatBridge);
            return "true";
            });

        w.bind("hi5PostChatMessageToWindow", [chatBridge](std::string req) -> std::string {
            const std::string msgJson = ParseFirstJsonStringArg(req);
            if (!msgJson.empty()) {
                PostMessageToChatWindow(chatBridge, msgJson);
            }
            return "true";
            });

        w.bind("hi5CloseChatWindow", [chatBridge](std::string) -> std::string {
            CloseChatWindow(chatBridge);
            return "true";
            });

        w.bind("hi5OpenFileBrowserWindow", [fileBridge](std::string) -> std::string {
            LogInfo("[viewer-files] open requested from renderer");
            OpenFileWindow(fileBridge);
            return "true";
            });

        w.bind("hi5PostFileMessageToWindow", [fileBridge](std::string req) -> std::string {
            const std::string msgJson = ParseFirstJsonStringArg(req);
            if (!msgJson.empty()) PostMessageToFileWindow(fileBridge, msgJson);
            return "true";
            });

        w.bind("hi5CloseFileBrowserWindow", [fileBridge](std::string) -> std::string {
            CloseFileWindow(fileBridge);
            return "true";
            });

        w.bind("hi5GetClipboardText", [](std::string) -> std::string { return std::string("\"") + JsonEscape(ReadLocalClipboardText()) + "\""; });
        w.bind("hi5ReadClipboardText", [](std::string) -> std::string { return std::string("\"") + JsonEscape(ReadLocalClipboardText()) + "\""; });
        w.bind("hi5SetClipboardText", [](std::string req) -> std::string { return WriteLocalClipboardText(ParseFirstJsonStringArg(req)) ? "true" : "false"; });
        w.bind("hi5WriteClipboardText", [](std::string req) -> std::string { return WriteLocalClipboardText(ParseFirstJsonStringArg(req)) ? "true" : "false"; });

        w.set_title("Hi5Central Viewer");
        w.set_size(1280, 800, WEBVIEW_HINT_NONE);
#ifdef _WIN32
        InstallViewerKeyboardHook(w);
#endif

        if (!std::filesystem::exists(indexPath)) {
            LogError("Missing index.html");
            w.set_html(BuildErrorHtml("Missing file:\n" + indexPath.string()));
            w.run();
            return 1;
        }

        if (!std::filesystem::exists(rendererPath)) {
            LogError("Missing renderer.js");
            w.set_html(BuildErrorHtml("Missing file:\n" + rendererPath.string()));
            w.run();
            return 1;
        }

        std::string html = ReadTextFileUtf8(indexPath);
        if (html.empty()) {
            LogError("Failed to read index.html");
            w.set_html(BuildErrorHtml("Failed to read:\n" + indexPath.string()));
            w.run();
            return 1;
        }

        std::string rendererJs = ReadTextFileUtf8(rendererPath);
        if (rendererJs.empty()) {
            LogError("Failed to read renderer.js");
            w.set_html(BuildErrorHtml("Failed to read:\n" + rendererPath.string()));
            w.run();
            return 1;
        }

        const std::string bootstrap = BuildBootstrapScript(launch);
        const std::string rendererInline = BuildInlineScriptTag(rendererJs);
        html = InjectScriptsIntoHtml(html, bootstrap, rendererInline);

        LogInfo("Loading local viewer shell via set_html with inlined renderer.js");
        w.set_html(html);
        w.run();

        CloseChatWindow(chatBridge);
        CloseFileWindow(fileBridge);
#ifdef _WIN32
        UninstallViewerKeyboardHook();
#endif
        {
            std::lock_guard<std::mutex> lock(chatBridge->mutex);
            chatBridge->mainWindow = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(fileBridge->mutex);
            fileBridge->mainWindow = nullptr;
        }

        return 0;
    }

} // namespace hi5