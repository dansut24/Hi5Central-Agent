#!/bin/bash
set -euo pipefail

CONTENTS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$(cd "$CONTENTS_DIR/.." && pwd)"
APP_PARENT="$(cd "$APP_DIR/.." && pwd)"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
INSTALLER="$RESOURCES_DIR/Install Hi5Central Agent.command"

if [[ ! -x "$INSTALLER" ]]; then
  /usr/bin/osascript -e 'display dialog "The Hi5Central installer payload is missing." with title "Hi5Central Agent" buttons {"OK"} default button "OK"'
  exit 1
fi

CONFIG_SOURCE=""
for candidate in   "$APP_PARENT/Hi5CentralDeployment.json"   "$HOME/Downloads/Hi5CentralDeployment.json"   "/Library/Application Support/Hi5Central/Deployment.json"; do
  if [[ -f "$candidate" ]]; then
    CONFIG_SOURCE="$candidate"
    break
  fi
done

if [[ -z "$CONFIG_SOURCE" ]]; then
  /usr/bin/osascript -e 'display dialog "Hi5CentralDeployment.json was not found. Download the deployment JSON from the Hi5Central portal and place it beside this app or in Downloads." with title "Hi5Central Agent" buttons {"OK"} default button "OK" with icon caution'
  exit 2
fi

/usr/bin/osascript - "$INSTALLER" "$CONFIG_SOURCE" <<'APPLESCRIPT'
on run argv
  set installerPath to item 1 of argv
  set configPath to item 2 of argv
  try
    do shell script (quoted form of installerPath) & " --config " & (quoted form of configPath) with administrator privileges
    display dialog "Hi5Central Agent installed and enrolled successfully." with title "Hi5Central Agent" buttons {"OK"} default button "OK"
  on error errorMessage number errorNumber
    display dialog "Hi5Central Agent could not be installed." & return & return & errorMessage with title "Hi5Central Agent" buttons {"OK"} default button "OK" with icon stop
    error number errorNumber
  end try
end run
APPLESCRIPT