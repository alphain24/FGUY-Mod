@echo off
rem Starts a test bot: a hidden, muted second copy of the game that goes to ONLINE, joins the first open
rem lobby (yours: host one first), picks a random character, readies up and plays the match by itself.
rem To join a private lobby, run:  "Start test bot.bat" ABCD   (the lobby's code)
rem "Stop test bot.bat" closes it again.
setlocal
cd /d "%~dp0.."
set FGUYMOD_BOT=1
set FGUYMOD_BOT_JOIN=%~1
set FGUYMOD_TESTINPUT=1
set FGUYMOD_TEST_AUTOMENU=online
set FGUYMOD_SAVEDIR=bot
set FGUYMOD_ONLINE_NAME=Bot
set FGUYMOD_ONLINE_CONTROLS=controller
set FGUYMOD_MODE=windowed
set FGUYMOD_RES=640x360
set FGUYMOD_BACKGROUND=1
set FGUYMOD_PADMODE=1
set FGUYMOD_NET_SCRIPT=1
start "" "FGUY.exe"
echo Test bot started. It joins the first open lobby in about a minute.
timeout /t 4 >nul 2>&1
