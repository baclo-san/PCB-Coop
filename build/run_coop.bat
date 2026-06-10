@echo off
rem One-click launcher for PCB co-op. MUST launch the game THIS way every time —
rem double-clicking th07.exe (or the Touhou07/custom launchers) runs WITHOUT the mod.
cd /d "%~dp0"
echo Launching th07.exe with th07_coop.dll injected...
injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe" "th07_coop.dll"
echo.
echo If you see "injected OK" above, the mod is loaded. Get into a stage and wait ~3s.
echo (You can close this window; the game keeps running.)
pause
