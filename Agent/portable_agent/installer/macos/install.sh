#!/bin/bash
set -euo pipefail

TOKEN=""
DEPLOYMENT_ID=""
DEPLOYMENT_SECRET=""
API_BASE="https://api.hi5central.com"
SOURCE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALL_DIR="/Library/Application Support/Hi5Central/Agent"
LOG_DIR="/Library/Logs/Hi5Central"
PLIST="/Library/LaunchDaemons/com.hi5central.agent.plist"
LABEL="com.hi5central.agent"
BINARY="$INSTALL_DIR/Hi5CentralAgent"
STATE_FILE="$INSTALL_DIR/agent.json"
REMOTE_HELPER_SOURCE="$SOURCE_DIR/Hi5Central Remote Helper.app"
REMOTE_HELPER_INSTALL="/Applications/Hi5Central Remote Helper.app"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --enrollment-token) TOKEN="${2:-}"; shift 2 ;;
    --deployment-id) DEPLOYMENT_ID="${2:-}"; shift 2 ;;
    --deployment-secret) DEPLOYMENT_SECRET="${2:-}"; shift 2 ;;
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

if [[ ! -x "$REMOTE_HELPER_SOURCE/Contents/MacOS/Hi5CentralRemoteHelper" ]]; then
  echo "Hi5Central Remote Helper.app is missing from this package." >&2
  exit 1
fi

mkdir -p "$INSTALL_DIR" "$LOG_DIR"
chmod 700 "$INSTALL_DIR"

echo "Preparing Hi5Central Agent..."
CANDIDATE_VERSION=""
if ! CANDIDATE_VERSION="$("$SOURCE_DIR/Hi5CentralAgent" --version 2>&1)"; then
  echo "The candidate Hi5Central Agent binary cannot run on this Mac." >&2
  echo "$CANDIDATE_VERSION" >&2
  exit 1
fi
echo "Candidate version: $CANDIDATE_VERSION"

if launchctl print "system/$LABEL" >/dev/null 2>&1; then
  echo "Stopping existing Hi5Central Agent service..."
  if [[ -f "$PLIST" ]]; then
    launchctl bootout system "$PLIST" >/dev/null 2>&1 ||       launchctl bootout "system/$LABEL" >/dev/null 2>&1 || true
  else
    launchctl bootout "system/$LABEL" >/dev/null 2>&1 || true
  fi

  for _ in 1 2 3 4 5; do
    if ! launchctl print "system/$LABEL" >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done

  if launchctl print "system/$LABEL" >/dev/null 2>&1; then
    echo "Unable to stop the existing Hi5Central Agent LaunchDaemon." >&2
    launchctl print "system/$LABEL" >&2 || true
    exit 1
  fi
fi

install -m 0755 "$SOURCE_DIR/Hi5CentralAgent" "$BINARY"

echo "Installing Hi5Central Remote Helper..."
/usr/bin/pkill -f '/Applications/Hi5Central Remote Helper.app/Contents/MacOS/Hi5CentralRemoteHelper' >/dev/null 2>&1 || true
rm -rf "$REMOTE_HELPER_INSTALL"
/usr/bin/ditto "$REMOTE_HELPER_SOURCE" "$REMOTE_HELPER_INSTALL"
chown -R root:wheel "$REMOTE_HELPER_INSTALL"
chmod 0755 "$REMOTE_HELPER_INSTALL/Contents/MacOS/Hi5CentralRemoteHelper"

if [[ -s "$STATE_FILE" ]]; then
  echo "Existing Hi5Central Agent identity found; preserving enrollment."
else
  if [[ -z "$TOKEN" && -z "$DEPLOYMENT_ID" ]]; then
    echo "--deployment-id or --enrollment-token is required for a new installation." >&2
    exit 2
  fi
  if [[ -n "$DEPLOYMENT_ID" && -z "$DEPLOYMENT_SECRET" ]]; then
    echo "--deployment-secret is required with --deployment-id." >&2
    exit 2
  fi

  echo "Enrolling Hi5Central Agent..."
  ENROLL_ARGS=(--state-dir "$INSTALL_DIR" --api-base "$API_BASE" --enroll-only)
  if [[ -n "$DEPLOYMENT_ID" ]]; then
    ENROLL_ARGS+=(--deployment-id "$DEPLOYMENT_ID" --deployment-secret "$DEPLOYMENT_SECRET")
  else
    ENROLL_ARGS+=(--enrollment-token "$TOKEN")
  fi
  "$BINARY" "${ENROLL_ARGS[@]}"
fi

PLIST_TMP="$(mktemp -t hi5central-agent-plist)"
cleanup() {
  rm -f "$PLIST_TMP"
}
trap cleanup EXIT

cat > "$PLIST_TMP" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$BINARY</string>
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

if ! /usr/bin/plutil -lint "$PLIST_TMP" >/dev/null; then
  echo "Generated LaunchDaemon plist is invalid." >&2
  /usr/bin/plutil -lint "$PLIST_TMP" >&2 || true
  exit 1
fi

install -o root -g wheel -m 0644 "$PLIST_TMP" "$PLIST"

echo "Starting Hi5Central Agent service..."
launchctl enable "system/$LABEL" >/dev/null 2>&1 || true
BOOTSTRAP_OUTPUT=""
if ! BOOTSTRAP_OUTPUT="$(launchctl bootstrap system "$PLIST" 2>&1)"; then
  echo "Hi5Central Agent LaunchDaemon bootstrap failed." >&2
  [[ -n "$BOOTSTRAP_OUTPUT" ]] && echo "$BOOTSTRAP_OUTPUT" >&2
  echo "--- launchctl service state ---" >&2
  launchctl print "system/$LABEL" >&2 || true
  echo "--- plist validation ---" >&2
  /usr/bin/plutil -lint "$PLIST" >&2 || true
  echo "--- recent Agent log ---" >&2
  tail -40 "$LOG_DIR/Agent.log" >&2 2>/dev/null || true
  exit 1
fi

launchctl kickstart -k "system/$LABEL"

sleep 1
if ! launchctl print "system/$LABEL" >/dev/null 2>&1; then
  echo "Hi5Central Agent service did not remain loaded after bootstrap." >&2
  tail -40 "$LOG_DIR/Agent.log" >&2 2>/dev/null || true
  exit 1
fi

echo
echo "Hi5Central Agent installed successfully."
echo "Version: $("$BINARY" --version 2>/dev/null || echo unknown)"
echo "Enrollment: preserved/new identity available"
echo "Service: system/$LABEL"
echo "Logs: tail -f '$LOG_DIR/Agent.log'"
echo "Code signing/notarisation will be added later; macOS may require explicit approval during testing."