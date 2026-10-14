@echo off
cd /d "%~dp0"
uv run --frozen python debugger.py gui %*
if errorlevel 1 pause
