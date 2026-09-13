from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Remove startup test message if it still exists.
text = re.sub(
    r'return L"\\"" \+ exe \+ L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass -Command \\"Write-Host .*?; powershell -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass\\"";',
    r'return L"\\"" + exe + L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass";',
    text,
    count=1,
    flags=re.S
)

# Insert cleaner before SendTerminalMessage.
if "CleanTerminalOutputForBrowser" not in text:
    marker = "            void SendTerminalMessage("
    helper = r'''            std::string CleanTerminalOutputForBrowser(const std::string& input) {
                std::string out;
                out.reserve(input.size());

                for (size_t i = 0; i < input.size(); ++i) {
                    unsigned char c = static_cast<unsigned char>(input[i]);

                    // Strip ANSI/VT escape sequences.
                    if (c == 0x1B) {
                        ++i;
                        if (i < input.size() && input[i] == '[') {
                            while (i < input.size()) {
                                unsigned char x = static_cast<unsigned char>(input[i]);
                                if (x >= 0x40 && x <= 0x7E) break;
                                ++i;
                            }
                        } else if (i < input.size() && input[i] == ']') {
                            while (i < input.size()) {
                                if (input[i] == '\a') break;
                                if (input[i] == 0x1B && i + 1 < input.size() && input[i + 1] == '\\') {
                                    ++i;
                                    break;
                                }
                                ++i;
                            }
                        }
                        continue;
                    }

                    // Drop C1 CSI bytes and common control characters.
                    if (c == 0x9B) {
                        while (i < input.size()) {
                            unsigned char x = static_cast<unsigned char>(input[i]);
                            if (x >= 0x40 && x <= 0x7E) break;
                            ++i;
                        }
                        continue;
                    }

                    if (c == '\r') continue;
                    if (c < 32 && c != '\n' && c != '\t' && c != '\b') continue;

                    out.push_back(static_cast<char>(c));
                }

                return out;
            }

'''
    if marker not in text:
        raise SystemExit("SendTerminalMessage marker not found")
    text = text.replace(marker, helper + marker, 1)

# Make SendTerminalMessage clean terminal_output before JSON/send.
old = '''            void SendTerminalMessage(const std::string& type, const std::string& sessionId, const std::string& dataOrError) {
                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"data", type == "terminal_output" ? dataOrError : ""},
                    {"error", type == "terminal_error" ? dataOrError : ""}
                };
'''

new = '''            void SendTerminalMessage(const std::string& type, const std::string& sessionId, const std::string& dataOrError) {
                const bool isOutput = type == "terminal_output";
                const std::string cleaned = isOutput ? CleanTerminalOutputForBrowser(dataOrError) : dataOrError;

                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"data", isOutput ? cleaned : ""},
                    {"error", type == "terminal_error" ? cleaned : ""}
                };
'''

if old not in text:
    raise SystemExit("SendTerminalMessage json block not found")

text = text.replace(old, new, 1)

# Fix logging byte count to use cleaned size if present.
text = text.replace(
    'std::to_string(dataOrError.size())',
    'std::to_string(cleaned.size())'
)

path.write_text(text, encoding="utf-8")
