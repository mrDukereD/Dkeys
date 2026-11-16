@echo off
rem build.cmd: build to obj\, ask running DKeys to exit (do exit; else kill), copy exe to DEST (default D:\Progs\DKeys), start it again. Not admin: asks UAC and reruns itself elevated.
setlocal
rem not admin: relaunch self elevated (UAC prompt), hidden, output goes to obj\build.log and is printed here, exit code via obj\build.done
fltmc >nul 2>&1 && goto :main
del /q "%~dp0obj\build.*.done" >nul 2>&1
del /q "%~dp0obj\build.*.log" >nul 2>&1
if not exist "%~dp0obj" mkdir "%~dp0obj"
set "BSELF=%~dpnx0"
set "BARGS=%*"
set "BLOG=%~dp0obj\build.%RANDOM%%RANDOM%.log"
set "BDONE=%~dp0obj\build.%RANDOM%%RANDOM%.done"
powershell -NoProfile -Command "$q=[char]34; $a='/v:on /c '+$q+$q+$env:BSELF+$q+' '+$env:BARGS+' > '+$q+$env:BLOG+$q+' 2>&1 & echo !errorlevel! > '+$q+$env:BDONE+$q+$q; Remove-Item $env:BDONE -ea 0; try { Start-Process cmd -Verb RunAs -WindowStyle Hidden -ArgumentList $a } catch { Write-Host 'UAC declined'; exit 1 }; for ($i=0; $i -lt 1200 -and -not (Test-Path $env:BDONE); $i++) { Start-Sleep -Milliseconds 500 }; if (Test-Path $env:BDONE) { exit ([int](Get-Content $env:BDONE)) } else { Write-Host 'timeout'; exit 1 }"
set "RC=%errorlevel%"
if exist "%BLOG%" type "%BLOG%"
exit /b %RC%
:main
if not defined DEST set "DEST=D:\Progs\DKeys"
set "VCV="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (echo Visual Studio C++ tools not found & exit /b 1)
call "%VCV%" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
where python >nul 2>&1 && python src\mkscripts.py
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
start "" "%DEST%\DKeys.exe" <nul >nul 2>&1
