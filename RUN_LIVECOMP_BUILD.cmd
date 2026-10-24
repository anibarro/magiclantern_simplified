@echo off
setlocal
cd /d "%~dp0"
wsl.exe bash build_tools/build_livecomp.sh
set "LC_BUILD_RESULT=%ERRORLEVEL%"
echo Build exit code: %LC_BUILD_RESULT%
exit /b %LC_BUILD_RESULT%
