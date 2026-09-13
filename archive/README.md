# Legacy development archive

This directory contains historical development artefacts moved out of the active Agent and Viewer source trees.

Archived on 2026-09-13 during the Hi5Central Agent hardening/CI pass.

The archive is intentionally not referenced by CMake, installer scripts, or GitHub Actions. It exists only to preserve useful development history while keeping the active Windows build tree clean.

Contents include:
- one-off patch/fix scripts used during earlier development
- timestamped `.bak` / `.backup-*` source snapshots
- obsolete signing/UIAccess test helpers
- accidental zero-byte shell/redirection artefacts
- old Viewer migration patch scripts

Do not add files from this directory back into a production build without reviewing them first. Current build inputs remain under `Agent/chatpass_agent`, `Viewer/chatpass_viewer`, `ci`, and `.github/workflows`.
