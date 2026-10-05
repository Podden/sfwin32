@echo off
rem Play the whole game -- station and missions -- with the 3dfx renderer,
rem CD music from game\SF\MUSIC and HD replacements from hd\ when present.
rem Extra arguments are passed on, e.g. --msaa 8 or --scale 4.
set "ROOT=%~dp0"
set "HD="
if exist "%ROOT%hd" set HD=--hd "%ROOT%hd"
start "" "%ROOT%port\build\Release\fightport.exe" --chain --3dfx --assets "%ROOT%game\SF" %HD% %*
