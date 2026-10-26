@echo off
rem Build XPan from Windows through WSL. Run from the repository root.
cd /d "%~dp0"
wsl.exe bash build_tools/build_xpan.sh
echo EXIT CODE %ERRORLEVEL%
pause
