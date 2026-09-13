from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

text = text.replace("signaling_->Send(msg.dump());", "signaling_->send(msg.dump());")

path.write_text(text, encoding="utf-8")
