from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Add includes if missing.
include_anchor = "#include <winhttp.h>"
if include_anchor in text and "#include <map>" not in text:
    text = text.replace(include_anchor, include_anchor + "\n#include <map>", 1)

# Add TerminalSession class before Worker if possible.
anchor = "        class Worker"
if anchor not in text:
    raise SystemExit("Could not find Worker class anchor")

terminal_code = r'''
        struct TerminalSession {
            std::string sessionId;
            HANDLE process = nullptr;
            HANDLE thread = nullptr;
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
            std::atomic<bool> running{ false };
            std::thread reader;
        };

'''

if "struct TerminalSession" not in text:
    text = text.replace(anchor, terminal_code + "\n" + anchor, 1)

# Add terminal members inside Worker private area. If no private marker exists, this may need manual placement.
member_anchor = "            std::atomic<bool> stop_{ false };"
members = r'''
            std::mutex terminalsMutex_;
            std::map<std::string, std::shared_ptr<TerminalSession>> terminals_;

'''
if members.strip() not in text:
    if member_anchor not in text:
        raise SystemExit("Worker member anchor not found")
    text = text.replace(member_anchor, members + member_anchor, 1)

# Add terminal methods before SendInventorySnapshotSafe.
method_anchor = """            void SendInventorySnapshotSafe(const AgentIdentity& ident) {
"""

terminal_methods = r'''
            std::wstring TerminalExecutablePath(const std::string& shell, const std::string& arch) {
                wchar_t windowsDir[MAX_PATH]{};
                GetWindowsDirectoryW(windowsDir, MAX_PATH);

                std::wstring base = windowsDir;

                if (arch == "x86") {
                    base += L"\\SysWOW64\\";
                } else {
                    base += L"\\System32\\";
                }

                if (shell == "cmd") {
                    return base + L"cmd.exe";
                }

                return base + L"WindowsPowerShell\\v1.0\\powershell.exe";
            }

            std::wstring TerminalCommandLine(const std::string& shell, const std::string& arch) {
                std::wstring exe = TerminalExecutablePath(shell, arch);

                if (shell == "cmd") {
                    return L"\"" + exe + L"\"";
                }

                return L"\"" + exe + L"\" -NoLogo -NoExit -ExecutionPolicy Bypass";
            }

            void SendTerminalMessage(const std::string& type, const std::string& sessionId, const std::string& dataOrError) {
                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"data", type == "terminal_output" ? dataOrError : ""},
                    {"error", type == "terminal_error" ? dataOrError : ""}
                };

                try {
                    if (signaling_) {
                        signaling_->Send(msg.dump());
                    }
                } catch (...) {}
            }

            void StopTerminalSession(const std::string& sessionId) {
                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) return;
                    session = it->second;
                    terminals_.erase(it);
                }

                session->running.store(false);

                if (session->stdinWrite) {
                    CloseHandle(session->stdinWrite);
                    session->stdinWrite = nullptr;
                }

                if (session->process) {
                    TerminateProcess(session->process, 0);
                    CloseHandle(session->process);
                    session->process = nullptr;
                }

                if (session->thread) {
                    CloseHandle(session->thread);
                    session->thread = nullptr;
                }

                if (session->stdoutRead) {
                    CloseHandle(session->stdoutRead);
                    session->stdoutRead = nullptr;
                }

                SendTerminalMessage("terminal_closed", sessionId, "");
            }

            void StartTerminalSession(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string shell = msg.value("shell", std::string("powershell"));
                const std::string runAs = msg.value("runAs", msg.value("run_as", std::string("admin")));
                const std::string arch = msg.value("arch", std::string("x64"));

                if (sessionId.empty()) return;

                if (runAs == "user") {
                    SendTerminalMessage("terminal_error", sessionId, "Signed-in user terminal mode is coming next. Admin/service mode is available now.");
                    return;
                }

                StopTerminalSession(sessionId);

                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                sa.lpSecurityDescriptor = nullptr;

                HANDLE stdinRead = nullptr;
                HANDLE stdinWrite = nullptr;
                HANDLE stdoutRead = nullptr;
                HANDLE stdoutWrite = nullptr;

                if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
                    SendTerminalMessage("terminal_error", sessionId, "CreatePipe stdin failed");
                    return;
                }

                if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
                    CloseHandle(stdinRead);
                    CloseHandle(stdinWrite);
                    SendTerminalMessage("terminal_error", sessionId, "CreatePipe stdout failed");
                    return;
                }

                SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
                SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                si.hStdInput = stdinRead;
                si.hStdOutput = stdoutWrite;
                si.hStdError = stdoutWrite;

                PROCESS_INFORMATION pi{};

                std::wstring cmdLine = TerminalCommandLine(shell, arch);

                BOOL ok = CreateProcessW(
                    nullptr,
                    cmdLine.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi
                );

                CloseHandle(stdinRead);
                CloseHandle(stdoutWrite);

                if (!ok) {
                    DWORD err = GetLastError();
                    CloseHandle(stdinWrite);
                    CloseHandle(stdoutRead);
                    SendTerminalMessage("terminal_error", sessionId, "CreateProcess failed err=" + std::to_string(err));
                    return;
                }

                auto session = std::make_shared<TerminalSession>();
                session->sessionId = sessionId;
                session->process = pi.hProcess;
                session->thread = pi.hThread;
                session->stdinWrite = stdinWrite;
                session->stdoutRead = stdoutRead;
                session->running.store(true);

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    terminals_[sessionId] = session;
                }

                SendTerminalMessage("terminal_output", sessionId, "[Hi5Central] Admin terminal started.\r\n");

                session->reader = std::thread([this, session]() {
                    char buffer[4096];

                    while (session->running.load()) {
                        DWORD read = 0;
                        BOOL ok = ReadFile(session->stdoutRead, buffer, sizeof(buffer) - 1, &read, nullptr);

                        if (!ok || read == 0) {
                            break;
                        }

                        buffer[read] = '\0';
                        SendTerminalMessage("terminal_output", session->sessionId, std::string(buffer, read));
                    }

                    session->running.store(false);
                    SendTerminalMessage("terminal_closed", session->sessionId, "");
                });

                session->reader.detach();
            }

            void SendTerminalInput(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string data = msg.value("data", std::string());

                if (sessionId.empty()) return;

                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) {
                        SendTerminalMessage("terminal_error", sessionId, "Terminal session not found");
                        return;
                    }
                    session = it->second;
                }

                if (!session->stdinWrite) return;

                DWORD written = 0;
                WriteFile(session->stdinWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            }

            void HandleTerminalMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());

                if (type == "terminal_start") {
                    StartTerminalSession(msg);
                    return;
                }

                if (type == "terminal_input") {
                    SendTerminalInput(msg);
                    return;
                }

                if (type == "terminal_stop") {
                    const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                    StopTerminalSession(sessionId);
                    return;
                }

                if (type == "terminal_resize") {
                    return;
                }
            }

'''

if "void StartTerminalSession(const json& msg)" not in text:
    if method_anchor not in text:
        raise SystemExit("SendInventorySnapshotSafe method anchor not found")
    text = text.replace(method_anchor, terminal_methods + method_anchor, 1)

# Add dispatch in agent message handler.
# Put this near other message/action dispatches.
dispatch_marker = '''                if (type == "refresh_inventory" || type == "inventory_refresh") {'''
dispatch = '''                if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                    HandleTerminalMessage(msg);
                    return;
                }

'''
if dispatch.strip() not in text:
    if dispatch_marker in text:
        text = text.replace(dispatch_marker, dispatch + dispatch_marker, 1)
    else:
        raise SystemExit("Could not find action dispatch marker. Search for refresh_inventory block and I will tailor it.")

path.write_text(text, encoding="utf-8")
