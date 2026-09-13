from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''                    if ((it->second.received % (8ull * 1024ull * 1024ull)) < static_cast<unsigned long long>(written) ||
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

new = '''                    SendFileUploadJson({
                        {"type", "files_upload_progress"},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", it->second.filename},
                        {"index", msg.value("index", 0)},
                        {"receivedBytes", it->second.received},
                        {"received_bytes", it->second.received},
                        {"size", it->second.expected},
                        {"size_bytes", it->second.expected}
                    });

                    return;'''

if old not in text:
    raise SystemExit("Could not find reduced progress block")

text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
