from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# 1) Add live files send/handler before HandleTerminalMessage.
marker = "            void HandleTerminalMessage(const json& msg) {"

live_files_code = r'''
            void SendFilesMessage(
                const std::string& type,
                const std::string& sessionId,
                const std::string& path,
                const json& result,
                const std::string& errorMessage = std::string()
            ) {
                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"path", path},
                    {"result", result}
                };

                if (type == "files_result") {
                    if (result.contains("entries") && result["entries"].is_array()) {
                        msg["entries"] = result["entries"];
                    } else if (result.contains("files") && result["files"].is_array()) {
                        msg["entries"] = result["files"];
                    } else if (result.contains("items") && result["items"].is_array()) {
                        msg["entries"] = result["items"];
                    } else {
                        msg["entries"] = json::array();
                    }

                    if (result.contains("drives")) {
                        msg["drives"] = result["drives"];
                    }

                    if (result.contains("parent")) {
                        msg["parent"] = result["parent"];
                    }

                    if (result.contains("count")) {
                        msg["count"] = result["count"];
                    }
                }

                if (type == "files_error") {
                    msg["error"] = errorMessage.empty() ? "File listing failed" : errorMessage;
                }

                try {
                    if (signaling_) {
                        LogI("files websocket send type=" + type +
                             " session=" + sessionId +
                             " path=" + path +
                             " entries=" + std::to_string(msg.value("entries", json::array()).size()));
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("files websocket send failed type=" + type + " session=" + sessionId);
                }
            }

            void HandleLiveFilesMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                std::string targetPath = msg.value("path", msg.value("filePath", std::string("C:\\")));

                if (sessionId.empty()) {
                    LogW("live files message missing session id type=" + type);
                    return;
                }

                if (targetPath.empty()) {
                    targetPath = "C:\\";
                }

                if (type == "files_cancel") {
                    LogI("live files cancel requested session=" + sessionId);
                    return;
                }

                if (type != "files_list") {
                    LogW("unknown live files message type=" + type + " session=" + sessionId);
                    return;
                }

                LogI("live files list requested session=" + sessionId + " path=" + targetPath);

                std::thread([this, sessionId, targetPath]() {
                    try {
                        json payload = {
                            {"path", targetPath}
                        };

                        CommandResult cr = RunPowerShellCommand(
                            std::string("live-files-") + sessionId,
                            BuildFilesListScript(payload),
                            60
                        );

                        json result = BuildCommandActionResult(std::string(), cr);

                        const bool ok =
                            cr.error.empty() &&
                            cr.exitCode == 0 &&
                            result.value("status", std::string("ok")) != "failed";

                        if (!ok) {
                            std::string error = cr.error;
                            if (error.empty() && result.contains("error")) {
                                error = result.value("error", std::string());
                            }
                            if (error.empty()) {
                                error = "File listing failed";
                            }

                            LogW("live files list failed session=" + sessionId +
                                 " path=" + targetPath +
                                 " error=" + error);

                            SendFilesMessage("files_error", sessionId, targetPath, json::object(), error);
                            return;
                        }

                        const std::string resultPath = result.value("path", targetPath);

                        LogI("live files list completed session=" + sessionId +
                             " path=" + resultPath +
                             " entries=" + std::to_string(result.value("entries", json::array()).size()));

                        SendFilesMessage("files_result", sessionId, resultPath, result);
                    } catch (const std::exception& ex) {
                        LogW(std::string("live files exception session=") + sessionId + " error=" + ex.what());
                        SendFilesMessage("files_error", sessionId, targetPath, json::object(), ex.what());
                    } catch (...) {
                        LogW("live files unknown exception session=" + sessionId);
                        SendFilesMessage("files_error", sessionId, targetPath, json::object(), "Unknown file listing error");
                    }
                }).detach();
            }

'''

if "void HandleLiveFilesMessage(const json& msg)" not in text:
    if marker not in text:
        raise SystemExit("Could not find HandleTerminalMessage marker")
    text = text.replace(marker, live_files_code + marker, 1)

# 2) Route files_list/files_cancel before normal remote-control handling.
old = '''                    if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                    LogI("routing terminal websocket message type=" + type);
                    HandleTerminalMessage(msg);
                    return;
                }'''

new = '''                    if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                        LogI("routing terminal websocket message type=" + type);
                        HandleTerminalMessage(msg);
                        return;
                    }

                    if (type == "files_list" || type == "files_cancel") {
                        LogI("routing live files websocket message type=" + type);
                        HandleLiveFilesMessage(msg);
                        return;
                    }'''

if old in text:
    text = text.replace(old, new, 1)
elif 'HandleLiveFilesMessage(msg);' not in text:
    # More tolerant fallback around the terminal route.
    text = text.replace(
        '''                    HandleTerminalMessage(msg);
                    return;
                }''',
        '''                    HandleTerminalMessage(msg);
                    return;
                }

                    if (type == "files_list" || type == "files_cancel") {
                        LogI("routing live files websocket message type=" + type);
                        HandleLiveFilesMessage(msg);
                        return;
                    }''',
        1
    )

path.write_text(text, encoding="utf-8")
