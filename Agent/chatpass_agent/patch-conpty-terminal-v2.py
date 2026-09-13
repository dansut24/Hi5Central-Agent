from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Add missing includes near the existing map include if possible.
if "#include <map>" in text:
    if "#include <vector>" not in text:
        text = text.replace("#include <map>", "#include <map>\n#include <vector>", 1)
    if "#include <memory>" not in text:
        text = text.replace("#include <vector>", "#include <vector>\n#include <memory>", 1)
else:
    # fallback near winhttp
    marker = "#include <winhttp.h>"
    if marker in text:
        extra = marker
        if "#include <map>" not in text:
            extra += "\n#include <map>"
        if "#include <vector>" not in text:
            extra += "\n#include <vector>"
        if "#include <memory>" not in text:
            extra += "\n#include <memory>"
        text = text.replace(marker, extra, 1)

# Replace TerminalSession struct with a brace-count parser.
struct_start = text.find("struct TerminalSession")
if struct_start == -1:
    # Insert it before class Worker if it never got added.
    worker_anchor = "        class Worker"
    if worker_anchor not in text:
        raise SystemExit("Could not find TerminalSession or Worker anchor")
    new_struct = '''        struct TerminalSession {
            std::string sessionId;
            HPCON pseudoConsole = nullptr;
            HANDLE process = nullptr;
            HANDLE thread = nullptr;
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
            std::atomic<bool> running{ false };
            std::thread reader;
        };

'''
    text = text.replace(worker_anchor, new_struct + worker_anchor, 1)
else:
    brace_start = text.find("{", struct_start)
    if brace_start == -1:
        raise SystemExit("TerminalSession brace start not found")

    depth = 0
    pos = brace_start
    while pos < len(text):
        ch = text[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                # include following semicolon and any immediate whitespace/newlines
                end = pos + 1
                while end < len(text) and text[end].isspace():
                    end += 1
                if end < len(text) and text[end] == ";":
                    end += 1
                while end < len(text) and text[end] in " \t\r\n":
                    end += 1
                break
        pos += 1
    else:
        raise SystemExit("TerminalSession brace end not found")

    new_struct = '''        struct TerminalSession {
            std::string sessionId;
            HPCON pseudoConsole = nullptr;
            HANDLE process = nullptr;
            HANDLE thread = nullptr;
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
            std::atomic<bool> running{ false };
            std::thread reader;
        };

'''
    text = text[:struct_start] + new_struct + text[end:]

# Replace terminal method block.
start = text.find("            std::wstring TerminalExecutablePath(")
end = text.find("            void SendInventorySnapshotSafe", start)

if start == -1 or end == -1:
    raise SystemExit("Could not find terminal method block")

new_methods = r'''            std::wstring TerminalExecutablePath(const std::string& shell, const std::string& arch) {
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
                    {"session_id", sessionId},
                    {"data", type == "terminal_output" ? dataOrError : ""},
                    {"error", type == "terminal_error" ? dataOrError : ""}
                };

                try {
                    if (signaling_) {
                        signaling_->send(msg.dump());
                    }
                } catch (...) {}
            }

            static bool CreatePipePair(HANDLE* readPipe, HANDLE* writePipe, bool inheritRead, bool inheritWrite) {
                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                sa.lpSecurityDescriptor = nullptr;

                if (!CreatePipe(readPipe, writePipe, &sa, 0)) {
                    return false;
                }

                if (!inheritRead) {
                    SetHandleInformation(*readPipe, HANDLE_FLAG_INHERIT, 0);
                }

                if (!inheritWrite) {
                    SetHandleInformation(*writePipe, HANDLE_FLAG_INHERIT, 0);
                }

                return true;
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

                if (session->pseudoConsole) {
                    ClosePseudoConsole(session->pseudoConsole);
                    session->pseudoConsole = nullptr;
                }

                if (session->stdoutRead) {
                    CloseHandle(session->stdoutRead);
                    session->stdoutRead = nullptr;
                }

                SendTerminalMessage("terminal_closed", sessionId, "");
            }

            bool StartConPtyProcess(
                const std::string& sessionId,
                const std::string& shell,
                const std::string& arch,
                short cols,
                short rows,
                std::shared_ptr<TerminalSession>& sessionOut,
                std::string& errorOut
            ) {
                HANDLE inputRead = nullptr;
                HANDLE inputWrite = nullptr;
                HANDLE outputRead = nullptr;
                HANDLE outputWrite = nullptr;

                if (!CreatePipePair(&inputRead, &inputWrite, true, false)) {
                    errorOut = "CreatePipe input failed err=" + std::to_string(GetLastError());
                    return false;
                }

                if (!CreatePipePair(&outputRead, &outputWrite, false, true)) {
                    CloseHandle(inputRead);
                    CloseHandle(inputWrite);
                    errorOut = "CreatePipe output failed err=" + std::to_string(GetLastError());
                    return false;
                }

                COORD size{};
                size.X = cols > 0 ? cols : 120;
                size.Y = rows > 0 ? rows : 32;

                HPCON hpc = nullptr;
                HRESULT hr = CreatePseudoConsole(size, inputRead, outputWrite, 0, &hpc);

                CloseHandle(inputRead);
                CloseHandle(outputWrite);

                if (FAILED(hr)) {
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    char buf[64]{};
                    sprintf_s(buf, "0x%08lx", static_cast<unsigned long>(hr));
                    errorOut = std::string("CreatePseudoConsole failed hr=") + buf;
                    return false;
                }

                STARTUPINFOEXW si{};
                si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

                SIZE_T attrListSize = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);

                std::vector<char> attrListBuffer(attrListSize);
                si.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrListBuffer.data());

                if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize)) {
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "InitializeProcThreadAttributeList failed err=" + std::to_string(GetLastError());
                    return false;
                }

                if (!UpdateProcThreadAttribute(
                    si.lpAttributeList,
                    0,
                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                    hpc,
                    sizeof(HPCON),
                    nullptr,
                    nullptr
                )) {
                    DeleteProcThreadAttributeList(si.lpAttributeList);
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "UpdateProcThreadAttribute failed err=" + std::to_string(GetLastError());
                    return false;
                }

                PROCESS_INFORMATION pi{};
                std::wstring cmdLine = TerminalCommandLine(shell, arch);

                BOOL ok = CreateProcessW(
                    nullptr,
                    cmdLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si.StartupInfo,
                    &pi
                );

                DeleteProcThreadAttributeList(si.lpAttributeList);

                if (!ok) {
                    DWORD err = GetLastError();
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "CreateProcess terminal failed err=" + std::to_string(err);
                    return false;
                }

                auto session = std::make_shared<TerminalSession>();
                session->sessionId = sessionId;
                session->pseudoConsole = hpc;
                session->process = pi.hProcess;
                session->thread = pi.hThread;
                session->stdinWrite = inputWrite;
                session->stdoutRead = outputRead;
                session->running.store(true);

                sessionOut = session;
                return true;
            }

            void StartTerminalSession(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string shell = msg.value("shell", std::string("powershell"));
                const std::string runAs = msg.value("runAs", msg.value("run_as", std::string("admin")));
                const std::string arch = msg.value("arch", std::string("x64"));
                const short cols = static_cast<short>(msg.value("cols", 120));
                const short rows = static_cast<short>(msg.value("rows", 32));

                LogI("terminal start requested session=" + sessionId + " shell=" + shell + " runAs=" + runAs + " arch=" + arch);

                if (sessionId.empty()) return;

                if (runAs == "user") {
                    SendTerminalMessage("terminal_error", sessionId, "Signed-in user terminal mode is coming next. Admin/service mode is available now.");
                    return;
                }

                StopTerminalSession(sessionId);

                std::shared_ptr<TerminalSession> session;
                std::string error;

                if (!StartConPtyProcess(sessionId, shell, arch, cols, rows, session, error)) {
                    LogW("terminal start failed session=" + sessionId + " error=" + error);
                    SendTerminalMessage("terminal_error", sessionId, error);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    terminals_[sessionId] = session;
                }

                LogI("terminal started session=" + sessionId);

                session->reader = std::thread([this, session]() {
                    char buffer[8192];

                    while (session->running.load()) {
                        DWORD read = 0;
                        BOOL ok = ReadFile(session->stdoutRead, buffer, sizeof(buffer), &read, nullptr);

                        if (!ok || read == 0) {
                            break;
                        }

                        SendTerminalMessage("terminal_output", session->sessionId, std::string(buffer, read));
                    }

                    session->running.store(false);
                    SendTerminalMessage("terminal_closed", session->sessionId, "");
                    LogI("terminal reader exited session=" + session->sessionId);
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
                        LogW("terminal input received but session not found session=" + sessionId);
                        SendTerminalMessage("terminal_error", sessionId, "Terminal session not found");
                        return;
                    }
                    session = it->second;
                }

                if (!session->stdinWrite) return;

                DWORD written = 0;
                BOOL ok = WriteFile(session->stdinWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);

                if (!ok) {
                    DWORD err = GetLastError();
                    LogW("terminal input write failed session=" + sessionId + " err=" + std::to_string(err));
                    SendTerminalMessage("terminal_error", sessionId, "Terminal input failed err=" + std::to_string(err));
                    return;
                }

                LogI("terminal input written session=" + sessionId + " bytes=" + std::to_string(written));
            }

            void ResizeTerminalSession(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const short cols = static_cast<short>(msg.value("cols", 120));
                const short rows = static_cast<short>(msg.value("rows", 32));

                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) return;
                    session = it->second;
                }

                if (!session->pseudoConsole) return;

                COORD size{};
                size.X = cols > 0 ? cols : 120;
                size.Y = rows > 0 ? rows : 32;
                ResizePseudoConsole(session->pseudoConsole, size);
            }

            void HandleTerminalMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionIdForLog = msg.value("sessionId", msg.value("session_id", std::string()));

                LogI("terminal message received type=" + type + " session=" + sessionIdForLog);

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
                    ResizeTerminalSession(msg);
                    return;
                }
            }

'''

text = text[:start] + new_methods + text[end:]

path.write_text(text, encoding="utf-8")
