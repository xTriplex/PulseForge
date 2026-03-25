@echo off
setlocal

echo ============================================
echo Generating Visual Studio 2022 project files...
echo ============================================

REM Change to project root (in case script is run from elsewhere)
cd /d "%~dp0"

REM --------------------------------------------
REM Clean previous generated files
REM --------------------------------------------

echo.
echo Cleaning old project files...

if exist "*.sln" del /q *.sln
if exist "*.vcxproj" del /q *.vcxproj
if exist "*.vcxproj.filters" del /q *.vcxproj.filters
if exist "*.vcxproj.user" del /q *.vcxproj.user


if exist "Bin" rmdir /s /q Bin
if exist "Int" rmdir /s /q Int

REM --------------------------------------------
REM Generate with Premake
REM --------------------------------------------

echo.
echo Running Premake...

set PREMAKE_PATH="Vendor\Bin\premake5.exe"

if not exist %PREMAKE_PATH% (
echo ERROR: premake5.exe not found at %PREMAKE_PATH%
pause
exit /b 1
)

%PREMAKE_PATH% vs2022

if %ERRORLEVEL% neq 0 (
echo.
echo ERROR: Premake failed!
pause
exit /b 1
)

echo.
echo ============================================
echo Project files generated successfully!
echo ============================================

pause
endlocal
