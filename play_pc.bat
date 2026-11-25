@echo off
cd /d "%~dp0"
copy /Y "%~dp0renut_pc.toml" "%~dp0app\out\pc\renut.toml" >nul
app\out\pc\renut.exe --game_data_root="G:/!Project/!port/DmC - Devil May Cry/game" --user_data_root="G:/!Project/!port/DmC - Devil May Cry/dmc_devil-nx/out/pc_user" %*
