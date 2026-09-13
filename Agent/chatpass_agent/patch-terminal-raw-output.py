from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# If SendTerminalMessage currently creates `cleaned` using CleanTerminalOutputForBrowser,
# make it pass the raw ConPTY stream instead. xterm.js will render ANSI/VT properly.
text = re.sub(
    r'const bool isOutput = type == "terminal_output";\s*const std::string cleaned = isOutput \? CleanTerminalOutputForBrowser\(dataOrError\) : dataOrError;',
    'const bool isOutput = type == "terminal_output";\n                const std::string cleaned = dataOrError;',
    text,
    count=1
)

path.write_text(text, encoding="utf-8")
