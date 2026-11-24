@echo off
cd /d "%~dp0"
app\out\pc\renut.exe --config=renut_pc.toml %*
