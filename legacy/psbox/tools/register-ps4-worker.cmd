@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0register-ps4-worker.ps1"
if errorlevel 1 (
  echo.
  echo Registrazione fallita. Se manca il CLI: irm 'https://cursor.com/install?win32=true' ^| iex
  pause
  exit /b 1
)
echo.
pause
