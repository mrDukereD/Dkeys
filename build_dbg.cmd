@echo off
setlocal
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VCV%" >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
rc /nologo /i src /fo obj\dkeys.res src\dkeys.rc || exit /b 1
cl /nologo /utf-8 /O1 /Os /GS- /Gs1048576 /GR- /W3 /Zl /DDKDEBUG /Fo:obj\ /Fe:obj\DKeysDbg.exe src\dkeys.cpp obj\dkeys.res /link /SUBSYSTEM:WINDOWS /ENTRY:start /OPT:REF /OPT:ICF /FILEALIGN:512 /MERGE:.rdata=.text /NOLOGO
