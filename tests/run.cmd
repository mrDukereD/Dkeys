@echo off
rem tests\run.cmd: собирает и запускает проверку ядра календаря (src\calendar.h, calview.h, calsync.h; с настоящим calendars\stub.py, если найден python),
rem затем Python-проверки провайдеров (tests\sync_test.py; в нём --selftest у calendars\caldav.py, google.py, outlook.py). Результат C++ в %TEMP%\dkeys_caltest\.
setlocal
set "RC=0"
set "VCV="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (echo Visual Studio C++ tools not found & exit /b 1)
call "%VCV%" >nul 2>&1 || exit /b 1
set "OUT=%TEMP%\dkeys_caltest"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /utf-8 /W3 /Fo:"%OUT%\\" /Fe:"%OUT%\caltest.exe" "%~dp0caltest.cpp" || exit /b 1
"%OUT%\caltest.exe" || set "RC=1"
where python >nul 2>&1
if errorlevel 1 (
    echo python not found: Python tests skipped
) else (
    python "%~dp0sync_test.py" || set "RC=1"
)
exit /b %RC%
