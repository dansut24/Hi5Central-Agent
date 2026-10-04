#!/bin/bash
set -euo pipefail

DEPLOYMENT_ID="__DEPLOYMENT_ID__"
DEPLOYMENT_SECRET="__DEPLOYMENT_SECRET__"
API_BASE="__API_BASE__"
AGENT_URL="https://downloads.hi5central.com/agent/latest/Hi5CentralAgent-macOS-universal.tar.gz"

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

ENROLLMENT_TOKEN="$(/usr/bin/curl -fsSL -X POST \
  -H 'Content-Type: application/json' \
  -H 'Accept: text/plain' \
  --data "{\"deploymentSecret\":\"$DEPLOYMENT_SECRET\"}" \
  "$API_BASE/api/v1/agent/deployments/$DEPLOYMENT_ID/enrollment-token")"

if [[ -z "$ENROLLMENT_TOKEN" ]]; then
  echo "Hi5Central did not return an enrollment token." >&2
  exit 1
fi

/usr/bin/curl -fsSL "$AGENT_URL" -o "$TMP_DIR/agent.tar.gz"
/usr/bin/tar -xzf "$TMP_DIR/agent.tar.gz" -C "$TMP_DIR"
"$TMP_DIR/installer/macos/install.sh" \
  --enrollment-token "$ENROLLMENT_TOKEN" \
  --api-base "$API_BASE"