from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '''                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$base = " + PsSingleQuote(target) + "\\n" +
                        "$name = " + PsSingleQuote(nameValue) + "\\n" +
                        "$full = Join-Path -LiteralPath $base -ChildPath $name\\n" +
                        "New-Item -ItemType Directory -LiteralPath $full -Force | Out-Null\\n" +
                        "[pscustomobject]@{ status='ok'; action='mkdir'; path=$full } | ConvertTo-Json -Compress\\n";'''

new = '''                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$ErrorActionPreference = 'Stop'\\n" +
                        "$base = " + PsSingleQuote(target) + "\\n" +
                        "$name = " + PsSingleQuote(nameValue) + "\\n" +
                        "$invalid = [System.IO.Path]::GetInvalidFileNameChars()\\n" +
                        "foreach ($ch in $invalid) { if ($name.Contains([string]$ch)) { throw 'Folder name contains invalid characters' } }\\n" +
                        "$full = [System.IO.Path]::Combine($base, $name)\\n" +
                        "[System.IO.Directory]::CreateDirectory($full) | Out-Null\\n" +
                        "[pscustomobject]@{ status='ok'; action='mkdir'; path=$full } | ConvertTo-Json -Compress\\n";'''

if old not in text:
    raise SystemExit("Could not find old mkdir PowerShell block")

text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
