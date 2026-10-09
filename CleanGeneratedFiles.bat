@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Keep this small launcher stable; all policy and validation live in the companion script.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0CleanGeneratedFiles.ps1"
set "PF_CLEAN_EXIT=%ERRORLEVEL%"
if not "%PF_CLEAN_EXIT%"=="0" echo Cleanup utility exited with code %PF_CLEAN_EXIT%.
exit /b %PF_CLEAN_EXIT%
