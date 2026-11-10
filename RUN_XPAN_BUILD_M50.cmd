@echo off
rem Build XPan for the Canon EOS M50 (1.1.0) from Windows through WSL. Run from the repository root.
cd /d "%~dp0"
wsl.exe env CAM=M50.110 bash build_tools/build_xpan.sh
echo EXIT CODE %ERRORLEVEL%
pause
