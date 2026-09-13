from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Replace PowerShell launch line with a plain prompt/function startup.
# This keeps the UI text the same, but makes PowerShell output much cleaner.
pattern = r'return L"\\"" \+ exe \+ L"\\" -NoLogo -NoExit.*?";'

replacement = r'''return L"\\"" + exe + L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass -Command \\"$Host.UI.RawUI.WindowTitle='Hi5Central Terminal'; function global:prompt { 'PS ' + (Get-Location).Path + '> ' }\\"";'''

# Only replace inside TerminalCommandLine by replacing first matching PowerShell return after cmd block.
idx = text.find('return L"\\"" + exe + L"\\" -NoLogo')
if idx == -1:
    raise SystemExit("PowerShell return line not found")

line_end = text.find("\n", idx)
old_line = text[idx:line_end]
text = text[:idx] + replacement + text[line_end:]

path.write_text(text, encoding="utf-8")
