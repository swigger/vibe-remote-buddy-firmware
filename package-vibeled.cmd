@echo off
setlocal
cd /d "%~dp0"
set "VIBELED_PACKAGE_PAUSE=1"
for %%A in (%*) do if /i "%%~A"=="--no-pause" set "VIBELED_PACKAGE_PAUSE=0"
python -c "import sys; sys.exit(sys.version_info < (3,10))" >nul 2>&1
if not errorlevel 1 (
    python "%~dp0tools\build_vibeled.py" %*
    goto finished
)
py -3 -c "import sys; sys.exit(sys.version_info < (3,10))" >nul 2>&1
if not errorlevel 1 (
    py -3 "%~dp0tools\build_vibeled.py" %*
    goto finished
)
echo Python 3.10 or newer is required. Install Python and try again.
if "%VIBELED_PACKAGE_PAUSE%"=="1" pause
exit /b 1
:finished
set "VIBELED_PACKAGE_RESULT=%errorlevel%"
if not "%VIBELED_PACKAGE_RESULT%"=="0" echo Build failed. See the error above.
if "%VIBELED_PACKAGE_PAUSE%"=="1" pause
exit /b %VIBELED_PACKAGE_RESULT%
