@echo off
setlocal
cd /d "%~dp0"
where node.exe >nul 2>nul
if errorlevel 1 (
  echo Node.js 20 or newer is required. Install it from https://nodejs.org/
  pause
  exit /b 1
)
title Vibe Keyboard Bridge - keep this window open
node.exe bridge.mjs
if errorlevel 1 pause
endlocal
