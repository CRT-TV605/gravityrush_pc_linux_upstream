@echo off
rem Opens the MediEvil launcher (needs Python 3 with Pillow: pip install pillow).
cd /d "%~dp0"
where pyw >nul 2>nul && (start "" pyw -3 launcher\gow3_launcher_win.py) || (start "" pythonw launcher\gow3_launcher_win.py)
