#!/usr/bin/env bash
set -euo pipefail

CONFIG_SOURCE="${HI5_DEPLOYMENT_CONFIG:-}"
API_BASE=""
DEPLOYMENT_ID=""
DEPLOYMENT_SECRET=""
AGENT_URL="https://downloads.hi5central.com/agent/latest/Hi5CentralAgent-linux-x64.tar.gz"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config|--config-url) CONFIG_SOURCE="${2:-}"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

if [[ -z "$CONFIG_SOURCE" ]]; then
  for candidate in     "$PWD/Hi5CentralDeployment.json"     "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/Hi5CentralDeployment.json"     "/etc/hi5central/deployment.json"     "/var/lib/hi5central/deployment.json"; do
    if [[ -f "$candidate" ]]; then
      CONFIG_SOURCE="$candidate"
      break
    fi
  done
fi

if [[ -z "$CONFIG_SOURCE" ]]; then
  echo "Hi5CentralDeployment.json was not found. Pass --config <path-or-https-url>." >&2
  exit 2
fi

CONFIG_FILE="$TMP_DIR/Hi5CentralDeployment.json"
if [[ "$CONFIG_SOURCE" =~ ^https:// ]]; then
  curl -fsSL --proto '=https' --tlsv1.2 "$CONFIG_SOURCE" -o "$CONFIG_FILE"
elif [[ "$CONFIG_SOURCE" =~ ^http:// ]]; then
  echo "Deployment configuration URLs must use HTTPS." >&2
  exit 2
else
  cp "$CONFIG_SOURCE" "$CONFIG_FILE"
fi

json_string() {
  local key="$1"
  sed -n 's/.*"'"$key"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" | head -n 1
}

SCHEMA_VERSION="$(sed -n 's/.*"schemaVersion"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$CONFIG_FILE" | head -n 1)"
API_BASE="$(json_string apiBase)"
DEPLOYMENT_ID="$(json_string deploymentId)"
DEPLOYMENT_SECRET="$(json_string deploymentSecret)"

if [[ "$SCHEMA_VERSION" != "1" ]]; then
  echo "Unsupported Hi5Central deployment configuration." >&2
  exit 2
fi
if [[ ! "$API_BASE" =~ ^https:// ]]; then
  echo "Deployment API base must use HTTPS." >&2
  exit 2
fi
if [[ ! "$DEPLOYMENT_ID" =~ ^[0-9a-fA-F-]{36}$ || -z "$DEPLOYMENT_SECRET" ]]; then
  echo "Deployment configuration is incomplete." >&2
  exit 2
fi

ENROLLMENT_TOKEN="$(curl -fsSL -X POST   -H 'Content-Type: application/json'   -H 'Accept: text/plain'   --data "{\"deploymentSecret\":\"$DEPLOYMENT_SECRET\"}"   "$API_BASE/api/v1/agent/deployments/$DEPLOYMENT_ID/enrollment-token")"

if [[ -z "$ENROLLMENT_TOKEN" ]]; then
  echo "Hi5Central did not return an enrollment token." >&2
  exit 1
fi

curl -fsSL --proto '=https' --tlsv1.2 "$AGENT_URL" -o "$TMP_DIR/agent.tar.gz"
tar -xzf "$TMP_DIR/agent.tar.gz" -C "$TMP_DIR"
"$TMP_DIR/installer/linux/install.sh"   --enrollment-token "$ENROLLMENT_TOKEN"   --api-base "$API_BASE"