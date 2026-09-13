from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = '"$full = Join-Path -LiteralPath $base -ChildPath $name\\n" +'
new = '"$full = Join-Path -Path $base -ChildPath $name\\n" +'

if old not in text:
    raise SystemExit("Could not find mkdir Join-Path -LiteralPath line")

text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
