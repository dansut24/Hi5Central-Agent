#!/bin/bash
set -euo pipefail

DEPLOYMENT_ID="__DEPLOYMENT_ID__"
DEPLOYMENT_SECRET="__DEPLOYMENT_SECRET__"
API_BASE="__API_BASE__"
AGENT_URL="https://downloads.hi5central.com/agent/latest/Hi5CentralAgent-macOS-universal.tar.gz"

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

/usr/bin/curl -fsSL "$AGENT_URL" -o "$TMP_DIR/agent.tar.gz"
/usr/bin/tar -xzf "$TMP_DIR/agent.tar.gz" -C "$TMP_DIR"
"$TMP_DIR/installer/macos/install.sh" \
  --deployment-id "$DEPLOYMENT_ID" \
  --deployment-secret "$DEPLOYMENT_SECRET" \
  --api-base "$API_BASE"
