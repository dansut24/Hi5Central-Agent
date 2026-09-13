from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''                        SendTerminalMessage("terminal_output", session->sessionId, std::string(buffer, read));
'''

new = '''                        LogI("terminal output read session=" + session->sessionId + " bytes=" + std::to_string(read));
                        SendTerminalMessage("terminal_output", session->sessionId, std::string(buffer, read));
'''

if old not in text:
    raise SystemExit("terminal output send line not found")

text = text.replace(old, new, 1)

old = '''                try {
                    if (signaling_) {
                        signaling_->send(msg.dump());
                    }
                } catch (...) {}
'''

new = '''                try {
                    if (signaling_) {
                        LogI("terminal websocket send type=" + type + " session=" + sessionId + " bytes=" + std::to_string(dataOrError.size()));
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("terminal websocket send failed type=" + type + " session=" + sessionId);
                }
'''

if old not in text:
    raise SystemExit("SendTerminalMessage send block not found")

text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
