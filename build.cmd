@echo off
rem Use a process-local policy only; do not change the user's execution policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
