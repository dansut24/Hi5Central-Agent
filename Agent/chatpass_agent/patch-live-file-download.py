from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Add pure C++ Base64 helper before SendFilesMessage.
marker = "            void SendFilesMessage("
base64_code = r'''
            std::string Base64EncodeBytes(const unsigned char* data, size_t len) {
                static const char* table =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

                std::string out;
                out.reserve(((len + 2) / 3) * 4);

                for (size_t i = 0; i < len; i += 3) {
                    const unsigned int b0 = data[i];
                    const unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
                    const unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;

                    const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;

                    out.push_back(table[(triple >> 18) & 0x3F]);
                    out.push_back(table[(triple >> 12) & 0x3F]);
                    out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
                    out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
                }

                return out;
            }

'''

if "Base64EncodeBytes" not in text:
    text = text.replace(marker, base64_code + marker, 1)

# Add download sender/handler before HandleLiveFilesMessage.
marker = "            void HandleLiveFilesMessage(const json& msg) {"
download_code = r'''
            void SendFileDownloadJson(const json& msg) {
                try {
                    if (signaling_) {
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("files download websocket send failed");
                }
            }

            void HandleLiveFileDownloadRequest(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string transferId = msg.value("transferId", msg.value("transfer_id", std::string()));
                const std::string path = msg.value("path", std::string());

                if (sessionId.empty() || transferId.empty() || path.empty()) {
                    LogW("files download request missing session/transfer/path");
                    return;
                }

                std::thread([this, sessionId, transferId, path]() {
                    const unsigned long long maxBytes = 100ull * 1024ull * 1024ull;
                    const DWORD chunkSize = 64 * 1024;

                    try {
                        DWORD attrs = GetFileAttributesA(path.c_str());
                        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "File not found or path is a folder"}
                            });
                            return;
                        }

                        HANDLE file = CreateFileA(
                            path.c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr
                        );

                        if (file == INVALID_HANDLE_VALUE) {
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "Could not open file. Error " + std::to_string(GetLastError())}
                            });
                            return;
                        }

                        LARGE_INTEGER size{};
                        if (!GetFileSizeEx(file, &size)) {
                            CloseHandle(file);
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "Could not read file size"}
                            });
                            return;
                        }

                        const unsigned long long totalBytes = static_cast<unsigned long long>(size.QuadPart);
                        if (totalBytes > maxBytes) {
                            CloseHandle(file);
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "File is larger than the current 100 MB download limit"}
                            });
                            return;
                        }

                        std::string filename = path;
                        size_t slash = filename.find_last_of("\\/");
                        if (slash != std::string::npos) {
                            filename = filename.substr(slash + 1);
                        }
                        if (filename.empty()) filename = "download.bin";

                        SendFileDownloadJson({
                            {"type", "files_download_start"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"filename", filename},
                            {"size", totalBytes},
                            {"size_bytes", totalBytes}
                        });

                        std::vector<unsigned char> buffer(chunkSize);
                        unsigned long long offset = 0;
                        uint32_t index = 0;

                        for (;;) {
                            DWORD read = 0;
                            BOOL ok = ReadFile(file, buffer.data(), chunkSize, &read, nullptr);

                            if (!ok) {
                                const DWORD err = GetLastError();
                                CloseHandle(file);
                                SendFileDownloadJson({
                                    {"type", "files_error"},
                                    {"sessionId", sessionId},
                                    {"session_id", sessionId},
                                    {"transferId", transferId},
                                    {"transfer_id", transferId},
                                    {"path", path},
                                    {"error", "File read failed. Error " + std::to_string(err)}
                                });
                                return;
                            }

                            if (read == 0) break;

                            const std::string encoded = Base64EncodeBytes(buffer.data(), static_cast<size_t>(read));

                            SendFileDownloadJson({
                                {"type", "files_download_chunk"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"index", index},
                                {"offset", offset},
                                {"bytes", read},
                                {"data", encoded}
                            });

                            offset += read;
                            index += 1;
                        }

                        CloseHandle(file);

                        SendFileDownloadJson({
                            {"type", "files_download_complete"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"filename", filename},
                            {"size", totalBytes},
                            {"size_bytes", totalBytes},
                            {"chunks", index}
                        });

                        LogI("files download completed session=" + sessionId +
                             " path=" + path +
                             " bytes=" + std::to_string(totalBytes) +
                             " chunks=" + std::to_string(index));
                    } catch (const std::exception& ex) {
                        SendFileDownloadJson({
                            {"type", "files_error"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"error", ex.what()}
                        });
                    } catch (...) {
                        SendFileDownloadJson({
                            {"type", "files_error"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"error", "Unknown file download error"}
                        });
                    }
                }).detach();
            }

'''

if "HandleLiveFileDownloadRequest" not in text:
    text = text.replace(marker, download_code + marker, 1)

# Route download request in live files handler.
old = '''                if (type == "files_cancel") {
                    LogI("live files cancel requested session=" + sessionId);
                    return;
                }'''

new = '''                if (type == "files_cancel" || type == "files_download_cancel") {
                    LogI("live files cancel requested session=" + sessionId);
                    return;
                }

                if (type == "files_download_request") {
                    HandleLiveFileDownloadRequest(msg);
                    return;
                }'''

text = text.replace(old, new, 1)

# Route download request from websocket dispatcher.
text = text.replace(
    'if (type == "files_list" || type == "files_cancel") {',
    'if (type == "files_list" || type == "files_cancel" || type == "files_download_request" || type == "files_download_cancel") {',
    1
)

path.write_text(text, encoding="utf-8")
