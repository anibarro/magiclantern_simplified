@echo off
rem Build XPan for the Canon 6D (1.1.6) from Windows through WSL. Run from the repository root.
cd /d "%~dp0"
wsl.exe env CAM=6D.116 bash build_tools/build_xpan.sh
echo EXIT CODE %ERRORLEVEL%
pause
