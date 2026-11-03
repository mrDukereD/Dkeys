@echo off
setlocal
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCV="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (echo Visual Studio C++ tools not found & exit /b 1)
call "%VCV%" >nul || exit /b 1
cd /d "%~dp0"
if not exist dist mkdir dist
if not exist obj mkdir obj
cl /nologo /utf-8 /O1 /Os /GS- /Gs1048576 /GR- /W3 /Zl /Fo:obj\ /Fe:dist\DKeys.exe src\dkeys.cpp ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:start /OPT:REF /OPT:ICF /FILEALIGN:512 /MERGE:.rdata=.text /NOLOGO
if errorlevel 1 exit /b 1
for %%f in (dist\DKeys.exe) do echo built %%f  %%~zf bytes
