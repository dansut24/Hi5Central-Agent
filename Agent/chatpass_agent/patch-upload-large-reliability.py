from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# 1) Reduce upload progress websocket spam. Replace per-chunk progress send with every ~8MB.
old = '''                    SendFileUploadJson({
                        {"type", "files_upload_progress"},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", it->second.filename},
                        {"receivedBytes", it->second.received},
                        {"received_bytes", it->second.received},
                        {"size", it->second.expected},
                        {"size_bytes", it->second.expected}
                    });

                    return;'''

new = '''                    if ((it->second.received % (8ull * 1024ull * 1024ull)) < static_cast<unsigned long long>(written) ||
                        it->second.received == it->second.expected) {
                        SendFileUploadJson({
                            {"type", "files_upload_progress"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"filename", it->second.filename},
                            {"receivedBytes", it->second.received},
                            {"received_bytes", it->second.received},
                            {"size", it->second.expected},
                            {"size_bytes", it->second.expected}
                        });
                    }

                    return;'''

if old not in text:
    raise SystemExit("Could not find upload per-chunk progress block")

text = text.replace(old, new, 1)

# 2) Add completion validation before moving the temp file.
old = '''                    FlushFileBuffers(it->second.handle);
                    CloseHandle(it->second.handle);
                    it->second.handle = INVALID_HANDLE_VALUE;

                    DeleteFileA(it->second.finalPath.c_str());

                    BOOL moved = MoveFileA(it->second.tempPath.c_str(), it->second.finalPath.c_str());'''

new = '''                    LogI("files upload complete requested session=" + sessionId +
                         " file=" + it->second.filename +
                         " received=" + std::to_string(it->second.received) +
                         " expected=" + std::to_string(it->second.expected));

                    if (it->second.expected > 0 && it->second.received != it->second.expected) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"refreshPath", it->second.directory},
                            {"refresh_path", it->second.directory},
                            {"error", "Upload incomplete. Received " + std::to_string(it->second.received) + " of " + std::to_string(it->second.expected) + " bytes"}
                        });
                        CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    FlushFileBuffers(it->second.handle);
                    CloseHandle(it->second.handle);
                    it->second.handle = INVALID_HANDLE_VALUE;

                    DeleteFileA(it->second.finalPath.c_str());

                    BOOL moved = MoveFileExA(
                        it->second.tempPath.c_str(),
                        it->second.finalPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED
                    );'''

if old not in text:
    raise SystemExit("Could not find upload complete move block")

text = text.replace(old, new, 1)

# 3) Replace old MoveFileA failure wording if present.
text = text.replace(
    '"Could not move uploaded file into place. Windows error " + std::to_string(GetLastError())',
    '"Could not finalise uploaded file. Windows error " + std::to_string(GetLastError())'
)

path.write_text(text, encoding="utf-8")
