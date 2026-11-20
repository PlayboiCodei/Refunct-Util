@echo off
setlocal EnableExtensions
call "%~dp0build_win32.bat" --no-pause
if errorlevel 1 (
    echo Build failed. No player package was generated.
    pause
    exit /b 1
)
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0release\Build-PlayerPackage.ps1" -AsiPath "%~dp0build-win32\Release\RefunctUtility.asi" -OutputDir "%~dp0dist"
if errorlevel 1 pause
