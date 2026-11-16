@echo off
setlocal
cd /d "%~dp0"
echo Hi5Central Streaming Lab - isolated remote test
echo The EXE will display a temporary code for https://hi5central.com/streamlab
echo The console must remain open while testing. Press Ctrl+C to stop.
echo.
"%~dp0Hi5StreamingLab.exe" --remote
echo.
echo Streaming Lab exited. Press any key to close.
pause >nul
