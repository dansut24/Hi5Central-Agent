from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''            void HandleTerminalMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());

                if (type == "terminal_start") {
'''

new = '''            void HandleTerminalMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionIdForLog = msg.value("sessionId", msg.value("session_id", std::string()));

                LogI("terminal message received type=" + type + " session=" + sessionIdForLog);

                if (type == "terminal_start") {
'''

if old not in text:
    raise SystemExit("HandleTerminalMessage block not found")

text = text.replace(old, new, 1)

old = '''                if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                    HandleTerminalMessage(msg);
                    return;
                }
'''

new = '''                if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                    LogI("routing terminal websocket message type=" + type);
                    HandleTerminalMessage(msg);
                    return;
                }
'''

if old in text:
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
