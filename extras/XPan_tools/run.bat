@echo off
rem Rebuild the XPan watermark from logo.png (needs: pip install pillow numpy).
rem The result goes to data\XPAN\WMARK.BIN, which the XPan build copies to ML\XPAN\ in the zip.
cd /d "%~dp0"
python xpan_watermark.py logo.png --out ..\..\data\XPAN\WMARK.BIN --preview
pause
