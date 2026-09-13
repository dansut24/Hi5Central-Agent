@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ROOT=C:\Users\Dan\Desktop\VPS\Agent\chatpass_agent"
set "VCPKG_ROOT=C:\vcpkg"
set "LIBDATACHANNEL_ROOT=C:\Users\Dan\Desktop\libdatachannel-install-static"
set "INNO=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"

set "CERT=C:\Users\Dan\Desktop\Hi5Central-Test-CodeSigning.pfx"
set "CERT_PASS=Hi5TestSigning123!"
set "TIMESTAMP=http://timestamp.digicert.com"

set "CONFIG=Release"
set "AGENT_VERSION=1.0.0"

set "BUILD_RELEASE_DIR=%ROOT%\build\%CONFIG%"
set "AGENT_EXE=%BUILD_RELEASE_DIR%\Hi5CentralAgent.exe"
set "ISS_FILE=%ROOT%\installer\windows\Hi5CentralAgentSetup.iss"
set "INSTALLER_EXE=%ROOT%\dist\installer\Hi5CentralAgentSetup.exe"

cd /d "%ROOT%"
if errorlevel 1 goto fail

echo.
echo ============================================================
echo Hi5Central Agent build/sign temporary SoftwareSASGeneration CAD
echo ============================================================
echo Root:        %ROOT%
echo Config:      %CONFIG%
echo Inno:        %INNO%
echo Cert:        %CERT%
echo Installer:   %INSTALLER_EXE%
echo ============================================================
echo.

echo Checking required tools...
where cmake >nul 2>nul
if errorlevel 1 goto fail_cmake
where signtool >nul 2>nul
if errorlevel 1 goto fail_signtool
if not exist "%INNO%" goto fail_inno
if not exist "%CERT%" goto fail_cert
if not exist "%ISS_FILE%" goto fail_iss

echo.
echo ============================================================
echo Clean build folder
echo ============================================================
rmdir /s /q "%ROOT%\build" 2>nul

echo.
echo ============================================================
echo Configure CMake
echo ============================================================
cmake -S "%ROOT%" -B "%ROOT%\build" ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=%CONFIG% ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DHI5_REQUIRE_STATIC_CRT=ON ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DLIBDATACHANNEL_ROOT="%LIBDATACHANNEL_ROOT%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

if errorlevel 1 goto fail_configure

echo.
echo ============================================================
echo Build native_vp8_stream
echo ============================================================
cmake --build "%ROOT%\build" --config %CONFIG% --target native_vp8_stream -j

if errorlevel 1 goto fail_agent_build
if not exist "%AGENT_EXE%" goto fail_agent_missing

echo.
echo ============================================================
echo Sign Hi5CentralAgent.exe
echo ============================================================
signtool sign /fd SHA256 ^
  /f "%CERT%" ^
  /p "%CERT_PASS%" ^
  /tr "%TIMESTAMP%" ^
  /td SHA256 ^
  "%AGENT_EXE%"

if errorlevel 1 goto fail_agent_sign

echo.
echo ============================================================
echo Verify signed EXE
echo ============================================================
signtool verify /pa /v "%AGENT_EXE%"
if errorlevel 1 goto fail_agent_verify

echo.
echo ============================================================
echo Build installer with Inno Setup directly
echo ============================================================
if not exist "%ROOT%\dist\installer" mkdir "%ROOT%\dist\installer"

"%INNO%" ^
  /DAppVersion="%AGENT_VERSION%" ^
  /DSourceDir="%BUILD_RELEASE_DIR%" ^
  /O"%ROOT%\dist\installer" ^
  /F"Hi5CentralAgentSetup" ^
  "%ISS_FILE%"

if errorlevel 1 goto fail_inno_build
if not exist "%INSTALLER_EXE%" goto fail_installer_missing

echo.
echo ============================================================
echo Sign final installer
echo ============================================================
signtool sign /fd SHA256 ^
  /f "%CERT%" ^
  /p "%CERT_PASS%" ^
  /tr "%TIMESTAMP%" ^
  /td SHA256 ^
  "%INSTALLER_EXE%"

if errorlevel 1 goto fail_installer_sign

echo.
echo ============================================================
echo Verify final installer
echo ============================================================
signtool verify /pa /v "%INSTALLER_EXE%"
if errorlevel 1 goto fail_installer_verify

echo.
echo ============================================================
echo SUCCESS
echo ============================================================
echo Agent EXE:
echo %AGENT_EXE%
echo.
echo Installer:
echo %INSTALLER_EXE%
echo.
goto done

:fail_cmake
echo ERROR: cmake not found in PATH.
goto fail
:fail_signtool
echo ERROR: signtool not found in PATH.
goto fail
:fail_inno
echo ERROR: Inno Setup compiler not found.
goto fail
:fail_cert
echo ERROR: Certificate PFX not found.
goto fail
:fail_iss
echo ERROR: ISS file not found.
goto fail
:fail_configure
echo ERROR: CMake configure failed.
goto fail
:fail_agent_build
echo ERROR: native_vp8_stream build failed.
goto fail
:fail_agent_missing
echo ERROR: Hi5CentralAgent.exe was not created.
goto fail
:fail_agent_sign
echo ERROR: signing Hi5CentralAgent.exe failed.
goto fail
:fail_agent_verify
echo ERROR: verification failed for Hi5CentralAgent.exe.
goto fail
:fail_inno_build
echo ERROR: Inno Setup build failed.
goto fail
:fail_installer_missing
echo ERROR: installer was not created.
goto fail
:fail_installer_sign
echo ERROR: signing final installer failed.
goto fail
:fail_installer_verify
echo ERROR: final installer verification failed.
goto fail

:fail
echo.
echo ============================================================
echo FAILED
echo ============================================================
echo The command window has been kept open so you can read the error above.
echo.

:done
pause
endlocal
