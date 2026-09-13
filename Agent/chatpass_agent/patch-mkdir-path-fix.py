from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''                if (refreshPath.empty()) {
                    refreshPath = "C:\\\\";
                }'''

new = '''                if (refreshPath.empty()) {
                    refreshPath = pathValue.empty() ? "C:\\\\" : pathValue;
                }'''

if old not in text:
    raise SystemExit("Could not find refreshPath fallback block")

text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
