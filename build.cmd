@echo off
rem build.cmd: build to obj\, ask running DKeys to exit (do exit; else kill), copy exe to DEST (default D:\Progs\DKeys), start it again. Run as administrator (DKeys runs elevated).
setlocal
if not defined DEST set "DEST=D:\Progs\DKeys"
set "VCV="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (echo Visual Studio C++ tools not found & exit /b 1)
call "%VCV%" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
rc /nologo /i src /fo obj\dkeys.res src\dkeys.rc || exit /b 1
cl /nologo /utf-8 /O1 /Os /GS- /Gs1048576 /GR- /W3 /Zl /Fo:obj\ /Fe:obj\DKeys.exe src\dkeys.cpp obj\dkeys.res ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:start /OPT:REF /OPT:ICF /FILEALIGN:512 /MERGE:.rdata=.text /NOLOGO
if errorlevel 1 exit /b 1
for %%f in (obj\DKeys.exe) do echo built %%f  %%~zf bytes
if not exist "%DEST%" mkdir "%DEST%"
rem попросить работающий DKeys выйти (нужен [Remote] Enabled=1), иначе убить; ждать до ~5 с
if exist "%DEST%\DKeys.exe" "%DEST%\DKeys.exe" do exit >nul 2>&1
for /l %%i in (1,1,10) do (tasklist /fi "imagename eq DKeys.exe" 2>nul | find /i "DKeys.exe" >nul && ping -n 2 127.0.0.1 >nul)
tasklist /fi "imagename eq DKeys.exe" 2>nul | find /i "DKeys.exe" >nul && (taskkill /f /im DKeys.exe >nul 2>&1 & ping -n 2 127.0.0.1 >nul)
copy /y obj\DKeys.exe "%DEST%\DKeys.exe" >nul || (echo copy to %DEST% failed - DKeys may run as admin, start build as administrator & exit /b 1)
if exist "%DEST%\calendars" (copy /y calendars\*.py "%DEST%\calendars\" >nul & copy /y calendars\README.md "%DEST%\calendars\" >nul)
echo installed %DEST%\DKeys.exe
start "" "%DEST%\DKeys.exe"
