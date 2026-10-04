#!/bin/bash
set -euo pipefail

RESOURCES_DIR="$(cd "$(dirname "$0")/../Resources" && pwd)"
INSTALLER="$RESOURCES_DIR/Install Hi5Central Agent.command"

if [[ ! -x "$INSTALLER" ]]; then
  /usr/bin/osascript -e 'display dialog "The Hi5Central installer payload is missing." with title "Hi5Central Agent" buttons {"OK"} default button "OK"'
  exit 1
fi

/usr/bin/osascript - "$INSTALLER" <<'APPLESCRIPT'
on run argv
  set installerPath to item 1 of argv
  try
    do shell script (quoted form of installerPath) with administrator privileges
    display dialog "Hi5Central Agent installed and enrolled successfully." with title "Hi5Central Agent" buttons {"OK"} default button "OK"
  on error errorMessage number errorNumber
    display dialog "Hi5Central Agent could not be installed." & return & return & errorMessage with title "Hi5Central Agent" buttons {"OK"} default button "OK" with icon stop
    error number errorNumber
  end try
end run
APPLESCRIPT
