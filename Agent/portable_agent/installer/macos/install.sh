#!/bin/bash
set -euo pipefail

TOKEN=""
API_BASE="https://api.hi5central.com"
SOURCE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALL_DIR="/Library/Application Support/Hi5Central/Agent"
LOG_DIR="/Library/Logs/Hi5Central"
PLIST="/Library/LaunchDaemons/com.hi5central.agent.plist"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --enrollment-token) TOKEN="${2:-}"; shift 2 ;;
    --api-base) API_BASE="${2:-}"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run this installer with sudo/root." >&2
  exit 1
fi

if [[ -z "$TOKEN" ]]; then
  echo "--enrollment-token is required." >&2
  exit 2
fi

if [[ ! -x "$SOURCE_DIR/Hi5CentralAgent" ]]; then
  echo "Hi5CentralAgent binary not found beside this installer." >&2
  exit 1
fi

mkdir -p "$INSTALL_DIR" "$LOG_DIR"
chmod 700 "$INSTALL_DIR"
install -m 0755 "$SOURCE_DIR/Hi5CentralAgent" "$INSTALL_DIR/Hi5CentralAgent"

"$INSTALL_DIR/Hi5CentralAgent"   --state-dir "$INSTALL_DIR"   --api-base "$API_BASE"   --enrollment-token "$TOKEN"   --enroll-only

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.hi5central.agent</string>
  <key>ProgramArguments</key>
  <array>
    <string>$INSTALL_DIR/Hi5CentralAgent</string>
    <string>--state-dir</string>
    <string>$INSTALL_DIR</string>
    <string>--api-base</string>
    <string>$API_BASE</string>
    <string>--service</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>$LOG_DIR/Agent.log</string>
  <key>StandardErrorPath</key>
  <string>$LOG_DIR/Agent.log</string>
  <key>ProcessType</key>
  <string>Background</string>
</dict>
</plist>
EOF

chown root:wheel "$PLIST"
chmod 0644 "$PLIST"

launchctl bootout system/com.hi5central.agent >/dev/null 2>&1 || true
launchctl bootstrap system "$PLIST"
launchctl enable system/com.hi5central.agent
launchctl kickstart -k system/com.hi5central.agent

echo
echo "Hi5Central Agent installed and enrolled."
echo "Logs: tail -f '$LOG_DIR/Agent.log'"
echo "Code signing/notarisation will be added later; macOS may require explicit approval during testing."
