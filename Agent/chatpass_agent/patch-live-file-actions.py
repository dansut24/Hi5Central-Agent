from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

marker = "            void HandleLiveFilesMessage(const json& msg) {"

code = r'''
            void SendFilesActionResult(
                const std::string& sessionId,
                const std::string& action,
                bool success,
                const std::string& refreshPath,
                const std::string& message,
                const std::string& error = std::string()
            ) {
                json out = {
                    {"type", "files_action_result"},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"action", action},
                    {"success", success},
                    {"refreshPath", refreshPath},
                    {"refresh_path", refreshPath},
                    {"message", message},
                    {"error", error}
                };

                try {
                    if (signaling_) {
                        signaling_->send(out.dump());
                    }
                } catch (...) {
                    LogW("files action result websocket send failed action=" + action + " session=" + sessionId);
                }
            }

            void HandleLiveFileActionRequest(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string pathValue = msg.value("path", std::string());
                const std::string nameValue = msg.value("name", std::string());
                std::string refreshPath = msg.value("currentPath", msg.value("current_path", std::string()));

                if (sessionId.empty()) return;

                if (refreshPath.empty()) {
                    refreshPath = "C:\\";
                }

                if (type == "files_mkdir_request") {
                    if (nameValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Folder name is required");
                        return;
                    }

                    const std::string target = refreshPath;
                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$base = " + PsSingleQuote(target) + "\n" +
                        "$name = " + PsSingleQuote(nameValue) + "\n" +
                        "$full = Join-Path -LiteralPath $base -ChildPath $name\n" +
                        "New-Item -ItemType Directory -LiteralPath $full -Force | Out-Null\n" +
                        "[pscustomobject]@{ status='ok'; action='mkdir'; path=$full } | ConvertTo-Json -Compress\n";

                    std::thread([this, sessionId, type, refreshPath, ps, nameValue]() {
                        CommandResult cr = RunPowerShellCommand("live-file-action-" + sessionId, ps, 60);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        SendFilesActionResult(
                            sessionId,
                            type,
                            ok,
                            refreshPath,
                            ok ? "Folder created" : "",
                            ok ? "" : (cr.error.empty() ? "Create folder failed" : cr.error)
                        );
                    }).detach();

                    return;
                }

                if (type == "files_rename_request") {
                    if (pathValue.empty() || nameValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Path and new name are required");
                        return;
                    }

                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$path = " + PsSingleQuote(pathValue) + "\n" +
                        "$name = " + PsSingleQuote(nameValue) + "\n" +
                        "Rename-Item -LiteralPath $path -NewName $name -Force\n" +
                        "[pscustomobject]@{ status='ok'; action='rename'; path=$path; name=$name } | ConvertTo-Json -Compress\n";

                    std::thread([this, sessionId, type, refreshPath, ps]() {
                        CommandResult cr = RunPowerShellCommand("live-file-action-" + sessionId, ps, 60);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        SendFilesActionResult(
                            sessionId,
                            type,
                            ok,
                            refreshPath,
                            ok ? "Renamed" : "",
                            ok ? "" : (cr.error.empty() ? "Rename failed" : cr.error)
                        );
                    }).detach();

                    return;
                }

                if (type == "files_delete_request") {
                    if (pathValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Path is required");
                        return;
                    }

                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$path = " + PsSingleQuote(pathValue) + "\n" +
                        "Remove-Item -LiteralPath $path -Force -Recurse\n" +
                        "[pscustomobject]@{ status='ok'; action='delete'; path=$path } | ConvertTo-Json -Compress\n";

                    std::thread([this, sessionId, type, refreshPath, ps]() {
                        CommandResult cr = RunPowerShellCommand("live-file-action-" + sessionId, ps, 60);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        SendFilesActionResult(
                            sessionId,
                            type,
                            ok,
                            refreshPath,
                            ok ? "Deleted" : "",
                            ok ? "" : (cr.error.empty() ? "Delete failed" : cr.error)
                        );
                    }).detach();

                    return;
                }

                SendFilesActionResult(sessionId, type, false, refreshPath, "", "Unknown file action");
            }

'''

if "HandleLiveFileActionRequest" not in text:
    text = text.replace(marker, code + marker, 1)

# Route inside live files handler before unknown type handling.
old = '''                if (type != "files_list") {
                    LogW("unknown live files message type=" + type + " session=" + sessionId);
                    return;
                }'''

new = '''                if (type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request") {
                    HandleLiveFileActionRequest(msg);
                    return;
                }

                if (type != "files_list") {
                    LogW("unknown live files message type=" + type + " session=" + sessionId);
                    return;
                }'''

text = text.replace(old, new, 1)

# Route from websocket dispatcher.
text = text.replace(
    'if (type == "files_list" || type == "files_cancel" || type == "files_download_request" || type == "files_download_cancel") {',
    'if (type == "files_list" || type == "files_cancel" || type == "files_download_request" || type == "files_download_cancel" || type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request") {',
    1
)

path.write_text(text, encoding="utf-8")
