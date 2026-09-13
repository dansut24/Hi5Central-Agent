from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Increase/remove current small download cap.
text = text.replace(
    "const unsigned long long maxBytes = 100ull * 1024ull * 1024ull;",
    "const unsigned long long maxBytes = 10ull * 1024ull * 1024ull * 1024ull;"
)

text = text.replace(
    '"File is larger than the current 100 MB download limit"',
    '"File is larger than the current 10 GB download limit"'
)

# Add Base64 decode helper after Base64EncodeBytes helper.
marker = "            void SendFileDownloadJson(const json& msg) {"
decode_helper = r'''
            std::vector<unsigned char> Base64DecodeBytes(const std::string& input) {
                static const int table[256] = {
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
                    -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
                    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
                };

                std::vector<unsigned char> out;
                int val = 0;
                int valb = -8;

                for (unsigned char c : input) {
                    int d = table[c];
                    if (d == -1) continue;
                    if (d == -2) break;

                    val = (val << 6) + d;
                    valb += 6;

                    if (valb >= 0) {
                        out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }

                return out;
            }

'''
if "Base64DecodeBytes" not in text:
    text = text.replace(marker, decode_helper + marker, 1)

# Add upload handler before HandleLiveFilesMessage.
marker = "            void HandleLiveFilesMessage(const json& msg) {"
upload_handler = r'''
            void SendFileUploadJson(const json& msg) {
                try {
                    if (signaling_) signaling_->send(msg.dump());
                } catch (...) {
                    LogW("files upload websocket send failed");
                }
            }

            void HandleLiveFileUploadMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string transferId = msg.value("transferId", msg.value("transfer_id", std::string()));
                const std::string directory = msg.value("directory", msg.value("path", std::string("C:\\")));
                const std::string filename = msg.value("filename", std::string());

                struct UploadState {
                    HANDLE handle = INVALID_HANDLE_VALUE;
                    std::string tempPath;
                    std::string finalPath;
                    std::string directory;
                    std::string filename;
                    unsigned long long received = 0;
                    unsigned long long expected = 0;
                };

                static std::mutex uploadMutex;
                static std::unordered_map<std::string, UploadState> uploads;

                if (sessionId.empty() || transferId.empty()) return;

                if (type == "files_upload_start") {
                    if (filename.empty()) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Filename is required"}
                        });
                        return;
                    }

                    if (filename.find("\\") != std::string::npos || filename.find("/") != std::string::npos || filename.find(":") != std::string::npos) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Invalid filename"}
                        });
                        return;
                    }

                    std::string base = directory.empty() ? "C:\\" : directory;
                    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += "\\";

                    const std::string finalPath = base + filename;
                    const std::string tempPath = finalPath + ".hi5upload-" + transferId + ".tmp";

                    HANDLE handle = CreateFileA(
                        tempPath.c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (handle == INVALID_HANDLE_VALUE) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Could not create upload temp file. Windows error " + std::to_string(GetLastError())}
                        });
                        return;
                    }

                    UploadState state;
                    state.handle = handle;
                    state.tempPath = tempPath;
                    state.finalPath = finalPath;
                    state.directory = directory;
                    state.filename = filename;
                    state.expected = static_cast<unsigned long long>(msg.value("size_bytes", msg.value("size", 0ull)));

                    {
                        std::lock_guard<std::mutex> lock(uploadMutex);
                        uploads[transferId] = state;
                    }

                    LogI("files upload started session=" + sessionId + " path=" + finalPath);

                    SendFileUploadJson({
                        {"type", "files_upload_progress"},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", filename},
                        {"receivedBytes", 0},
                        {"received_bytes", 0},
                        {"size", state.expected},
                        {"size_bytes", state.expected}
                    });

                    return;
                }

                if (type == "files_upload_chunk") {
                    std::string data = msg.value("data", std::string());
                    std::vector<unsigned char> bytes = Base64DecodeBytes(data);

                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it == uploads.end() || it->second.handle == INVALID_HANDLE_VALUE) return;

                    DWORD written = 0;
                    BOOL ok = WriteFile(
                        it->second.handle,
                        bytes.data(),
                        static_cast<DWORD>(bytes.size()),
                        &written,
                        nullptr
                    );

                    if (!ok || written != bytes.size()) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Upload write failed. Windows error " + std::to_string(GetLastError())}
                        });
                        CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    it->second.received += written;

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

                    return;
                }

                if (type == "files_upload_complete") {
                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it == uploads.end()) return;

                    FlushFileBuffers(it->second.handle);
                    CloseHandle(it->second.handle);
                    it->second.handle = INVALID_HANDLE_VALUE;

                    DeleteFileA(it->second.finalPath.c_str());

                    BOOL moved = MoveFileA(it->second.tempPath.c_str(), it->second.finalPath.c_str());
                    if (!moved) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"refreshPath", it->second.directory},
                            {"refresh_path", it->second.directory},
                            {"error", "Could not move uploaded file into place. Windows error " + std::to_string(GetLastError())}
                        });
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    LogI("files upload completed session=" + sessionId + " path=" + it->second.finalPath + " bytes=" + std::to_string(it->second.received));

                    SendFileUploadJson({
                        {"type", "files_upload_result"},
                        {"success", true},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", it->second.filename},
                        {"refreshPath", it->second.directory},
                        {"refresh_path", it->second.directory},
                        {"message", "Upload complete"}
                    });

                    uploads.erase(it);
                    return;
                }

                if (type == "files_upload_cancel") {
                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it != uploads.end()) {
                        if (it->second.handle != INVALID_HANDLE_VALUE) CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                    }
                    return;
                }
            }

'''
if "HandleLiveFileUploadMessage" not in text:
    text = text.replace(marker, upload_handler + marker, 1)

# Route upload messages inside live file handler before file actions.
if 'type == "files_upload_start"' not in text[text.find("void HandleLiveFilesMessage"):text.find("if (type == \"files_rename_request\"")]:
    text = text.replace(
      '''                if (type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request") {''',
      '''                if (type == "files_upload_start" || type == "files_upload_chunk" || type == "files_upload_complete" || type == "files_upload_cancel") {
                    HandleLiveFileUploadMessage(msg);
                    return;
                }

                if (type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request") {''',
      1
    )

# Route upload messages from websocket dispatcher.
text = text.replace(
  'type == "files_mkdir_request") {',
  'type == "files_mkdir_request" || type == "files_upload_start" || type == "files_upload_chunk" || type == "files_upload_complete" || type == "files_upload_cancel") {',
  1
)

path.write_text(text, encoding="utf-8")
