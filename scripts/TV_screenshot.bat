@echo off
rem Screenshot from the TV over ADB. Usage: screenshot.bat [tv_ip]   (default 192.168.0.7)
rem Needs "USB debugging" turned on in the TV's Developer options. The file goes to the "shots" folder next to this script.
setlocal
set "TV=%~1"
if "%TV%"=="" set "TV=192.168.0.7"
set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not exist "%ADB%" set "ADB=adb"
set "OUT=w:\DukereD\ClaudeProjects\TV\shots"
if not exist "%OUT%" mkdir "%OUT%"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%i"
set "FILE=%OUT%\tv-%STAMP%.png"

"%ADB%" connect %TV%:5555 >nul 2>&1
"%ADB%" -s %TV%:5555 get-state >nul 2>&1
if errorlevel 1 (
    echo Cannot reach the TV at %TV%:5555. Turn on USB debugging on the TV, check the IP, then try again.
    exit /b 1
)

"%ADB%" -s %TV%:5555 shell screencap -p /sdcard/tv-shot.png
"%ADB%" -s %TV%:5555 pull /sdcard/tv-shot.png "%FILE%" >nul
"%ADB%" -s %TV%:5555 shell rm -f /sdcard/tv-shot.png

if exist "%FILE%" (
    for %%f in ("%FILE%") do if %%~zf EQU 0 (
        echo Empty screenshot: the TV shows protected video ^(HDMI input, Netflix and the like^).
        del "%FILE%"
        exit /b 2
    )
    echo Saved: %FILE%
    start "" "%FILE%"
) else (
    echo Screenshot failed.
    exit /b 3
)
endlocal
