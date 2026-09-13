from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

text = text.replace(
'''                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,''',
'''                    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,''',
)

# Make PowerShell emit something immediately so we can prove output read works.
text = text.replace(
'''return L"\\"" + exe + L"\\" -NoLogo -NoExit -ExecutionPolicy Bypass";''',
'''return L"\\"" + exe + L"\\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass -Command \\"Write-Host '[Hi5Central] Admin terminal started.'; powershell -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass\\"";''',
)

path.write_text(text, encoding="utf-8")
