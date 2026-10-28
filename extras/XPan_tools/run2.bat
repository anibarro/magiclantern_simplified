@echo off
rem Rebuild the XPan watermark from logo_white.png (needs: pip install pillow numpy).
rem The result goes to data\XPAN\WMARK_2.BIN, which the XPan build copies to ML\XPAN\ in the zip.
cd /d "%~dp0"
python xpan_watermark.py logo_white.png --out ..\..\data\XPAN\WMARK_2.BIN --preview
pause
