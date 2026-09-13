from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''return L"\\"" + exe + L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass -Command \\"Write-Host '[Hi5Central] Admin terminal started.'; powershell -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass\\"";'''

new = '''return L"\\"" + exe + L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass";'''

if old not in text:
    raise SystemExit("startup test command not found")

text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
