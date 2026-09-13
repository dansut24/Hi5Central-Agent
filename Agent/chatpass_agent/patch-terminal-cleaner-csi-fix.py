from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

new_cleaner = r'''            std::string CleanTerminalOutputForBrowser(const std::string& input) {
                std::string out;
                out.reserve(input.size());

                for (size_t i = 0; i < input.size(); ++i) {
                    unsigned char c = static_cast<unsigned char>(input[i]);

                    // ESC sequences.
                    if (c == 0x1B) {
                        if (i + 1 < input.size() && input[i + 1] == '[') {
                            i += 2;
                            while (i < input.size()) {
                                unsigned char x = static_cast<unsigned char>(input[i]);
                                if (x >= 0x40 && x <= 0x7E) break;
                                ++i;
                            }
                            continue;
                        }

                        if (i + 1 < input.size() && input[i + 1] == ']') {
                            i += 2;
                            while (i < input.size()) {
                                if (input[i] == '\a') break;
                                if (input[i] == 0x1B && i + 1 < input.size() && input[i + 1] == '\\') {
                                    ++i;
                                    break;
                                }
                                ++i;
                            }
                            continue;
                        }

                        ++i;
                        continue;
                    }

                    // 8-bit CSI.
                    if (c == 0x9B) {
                        ++i;
                        while (i < input.size()) {
                            unsigned char x = static_cast<unsigned char>(input[i]);
                            if (x >= 0x40 && x <= 0x7E) break;
                            ++i;
                        }
                        continue;
                    }

                    // If an ESC was already stripped upstream, remove leftover CSI fragments.
                    if (c == '?' || c == '[' || (c >= '0' && c <= '9')) {
                        size_t j = i;
                        if (input[j] == '[') ++j;
                        if (j < input.size() && input[j] == '?') ++j;

                        bool sawDigit = false;
                        while (j < input.size() && ((input[j] >= '0' && input[j] <= '9') || input[j] == ';')) {
                            if (input[j] >= '0' && input[j] <= '9') sawDigit = true;
                            ++j;
                        }

                        if (sawDigit && j < input.size()) {
                            unsigned char finalChar = static_cast<unsigned char>(input[j]);
                            if ((finalChar >= 0x40 && finalChar <= 0x7E) || finalChar == 'h' || finalChar == 'l' || finalChar == 'm') {
                                i = j;
                                continue;
                            }
                        }
                    }

                    if (c == '\r') continue;
                    if (c < 32 && c != '\n' && c != '\t' && c != '\b') continue;

                    out.push_back(static_cast<char>(c));
                }

                return out;
            }'''

text = re.sub(
    r'            std::string CleanTerminalOutputForBrowser\(const std::string& input\) \{.*?            \}\n\n            void SendTerminalMessage',
    new_cleaner + "\n\n            void SendTerminalMessage",
    text,
    count=1,
    flags=re.S
)

path.write_text(text, encoding="utf-8")
