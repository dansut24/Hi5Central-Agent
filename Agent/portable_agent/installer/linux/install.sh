#!/usr/bin/env bash
set -euo pipefail

TOKEN=""
API_BASE="https://api.hi5central.com"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_DIR="/opt/hi5central/agent"
STATE_DIR="/var/lib/hi5central/agent"
SERVICE_FILE="/etc/systemd/system/hi5central-agent.service"
SERVICE_NAME="hi5central-agent.service"
BINARY="$INSTALL_DIR/Hi5CentralAgent"
STATE_FILE="$STATE_DIR/agent.json"
BACKUP_BINARY="$INSTALL_DIR/Hi5CentralAgent.previous"

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

if [[ ! -x "$SOURCE_DIR/Hi5CentralAgent" ]]; then
  echo "Hi5CentralAgent binary not found beside this installer." >&2
  exit 1
fi

echo "Preparing Hi5Central Agent..."
CANDIDATE_VERSION=""
if ! CANDIDATE_VERSION="$("$SOURCE_DIR/Hi5CentralAgent" --version 2>&1)"; then
  echo "The candidate Hi5Central Agent binary cannot run on this Linux host." >&2
  echo "$CANDIDATE_VERSION" >&2
  exit 1
fi
echo "Candidate version: $CANDIDATE_VERSION"

install -d -m 0755 "$INSTALL_DIR"
install -d -m 0700 "$STATE_DIR"

HAD_EXISTING_BINARY=false
if [[ -x "$BINARY" ]]; then
  HAD_EXISTING_BINARY=true
  cp -f "$BINARY" "$BACKUP_BINARY"
  chmod 0755 "$BACKUP_BINARY"
fi

if systemctl is-active --quiet "$SERVICE_NAME"; then
  echo "Stopping existing Hi5Central Agent service..."
  systemctl stop "$SERVICE_NAME"
fi

install -m 0755 "$SOURCE_DIR/Hi5CentralAgent" "$BINARY"

if [[ -s "$STATE_FILE" ]]; then
  echo "Existing Hi5Central Agent identity found; preserving enrollment."
else
  if [[ -z "$TOKEN" ]]; then
    echo "--enrollment-token is required for a new installation." >&2
    if [[ "$HAD_EXISTING_BINARY" == true && -x "$BACKUP_BINARY" ]]; then
      install -m 0755 "$BACKUP_BINARY" "$BINARY"
    fi
    exit 2
  fi

  echo "Enrolling Hi5Central Agent..."
  if ! "$BINARY"       --state-dir "$STATE_DIR"       --api-base "$API_BASE"       --enrollment-token "$TOKEN"       --enroll-only; then
    echo "Enrollment failed." >&2
    if [[ "$HAD_EXISTING_BINARY" == true && -x "$BACKUP_BINARY" ]]; then
      echo "Restoring previous Agent binary..." >&2
      install -m 0755 "$BACKUP_BINARY" "$BINARY"
    fi
    exit 1
  fi
fi

UNIT_TMP="$(mktemp --suffix=.service /tmp/hi5central-agent-XXXXXX)"
cleanup() {
  rm -f "$UNIT_TMP"
}
trap cleanup EXIT

cat > "$UNIT_TMP" <<EOF
[Unit]
Description=Hi5Central Agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$BINARY --state-dir $STATE_DIR --api-base $API_BASE --service
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

if command -v systemd-analyze >/dev/null 2>&1; then
  if ! systemd-analyze verify "$UNIT_TMP" >/dev/null 2>&1; then
    echo "Generated systemd unit failed validation." >&2
    systemd-analyze verify "$UNIT_TMP" >&2 || true
    if [[ "$HAD_EXISTING_BINARY" == true && -x "$BACKUP_BINARY" ]]; then
      echo "Restoring previous Agent binary and service..." >&2
      install -m 0755 "$BACKUP_BINARY" "$BINARY"
      systemctl daemon-reload >/dev/null 2>&1 || true
      systemctl restart "$SERVICE_NAME" >/dev/null 2>&1 || true
    fi
    exit 1
  fi
fi

install -m 0644 "$UNIT_TMP" "$SERVICE_FILE"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME" >/dev/null

echo "Starting Hi5Central Agent service..."
if ! systemctl restart "$SERVICE_NAME"; then
  echo "Hi5Central Agent failed to start." >&2
  systemctl --no-pager --full status "$SERVICE_NAME" >&2 || true
  journalctl -u "$SERVICE_NAME" -n 50 --no-pager >&2 || true

  if [[ "$HAD_EXISTING_BINARY" == true && -x "$BACKUP_BINARY" ]]; then
    echo "Restoring previous Agent binary..." >&2
    systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    install -m 0755 "$BACKUP_BINARY" "$BINARY"
    systemctl restart "$SERVICE_NAME" >/dev/null 2>&1 || true
  fi
  exit 1
fi

sleep 1
if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  echo "Hi5Central Agent did not remain active after restart." >&2
  systemctl --no-pager --full status "$SERVICE_NAME" >&2 || true
  journalctl -u "$SERVICE_NAME" -n 50 --no-pager >&2 || true

  if [[ "$HAD_EXISTING_BINARY" == true && -x "$BACKUP_BINARY" ]]; then
    echo "Restoring previous Agent binary..." >&2
    systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    install -m 0755 "$BACKUP_BINARY" "$BINARY"
    systemctl restart "$SERVICE_NAME" >/dev/null 2>&1 || true
  fi
  exit 1
fi

rm -f "$BACKUP_BINARY"

echo
echo "Hi5Central Agent installed successfully."
echo "Version: $("$BINARY" --version 2>/dev/null || echo unknown)"
echo "Enrollment: preserved/new identity available"
echo "Service: $SERVICE_NAME"
echo "Logs: journalctl -u $SERVICE_NAME -f"
