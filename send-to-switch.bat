@echo off
setlocal
title yuanshi launcher - Send NRO to Switch
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -STA -ExecutionPolicy RemoteSigned -File "%~dp0scripts\send-nro-to-switch.ps1" %*
set "YUANSHI_DEPLOY_EXIT=%ERRORLEVEL%"
if not "%YUANSHI_DEPLOY_NO_PAUSE%"=="1" pause
exit /b %YUANSHI_DEPLOY_EXIT%
