from pathlib import Path
import re

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# Detect the actual SignalingClient send method used elsewhere.
methods = re.findall(r"signaling_->([A-Za-z_][A-Za-z0-9_]*)\s*\(", text)
methods = [m for m in methods if m not in {"operator"}]

preferred = None
for candidate in ["SendText", "SendMessage", "SendJson", "SendRaw", "SendString", "Send"]:
    if candidate in methods:
        preferred = candidate
        break

if not preferred:
    print("Could not auto-detect signaling send method.")
    print("Found signaling methods:", sorted(set(methods)))
    raise SystemExit(1)

print("Using signaling send method:", preferred)

text = text.replace("signaling_->Send(msg.dump());", f"signaling_->{preferred}(msg.dump());")

path.write_text(text, encoding="utf-8")
