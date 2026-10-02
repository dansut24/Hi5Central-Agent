#!/usr/bin/env bash
set -euo pipefail

TOKEN=""
API_BASE="https://api.hi5central.com"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_DIR="/opt/hi5central/agent"
STATE_DIR="/var/lib/hi5central/agent"
SERVICE_FILE="/etc/systemd/system/hi5central-agent.service"

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

install -d -m 0755 "$INSTALL_DIR"
install -d -m 0700 "$STATE_DIR"
install -m 0755 "$SOURCE_DIR/Hi5CentralAgent" "$INSTALL_DIR/Hi5CentralAgent"

"$INSTALL_DIR/Hi5CentralAgent"   --state-dir "$STATE_DIR"   --api-base "$API_BASE"   --enrollment-token "$TOKEN"   --enroll-only

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Hi5Central Agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$INSTALL_DIR/Hi5CentralAgent --state-dir $STATE_DIR --api-base $API_BASE --service
Restart=always
RestartSec=5
User=root
Group=root
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
PrivateTmp=true
ReadWritePaths=$STATE_DIR

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now hi5central-agent.service
systemctl --no-pager --full status hi5central-agent.service || true

echo
echo "Hi5Central Agent installed and enrolled."
echo "Logs: journalctl -u hi5central-agent -f"
