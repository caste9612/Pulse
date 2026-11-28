@echo off
rem Compila Pulse 2 in out\Pulse2.exe. Serve Visual Studio con il compilatore C++ a 64 bit.
setlocal
cd /d "%~dp0"

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto build
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" echo vswhere non trovato: serve Visual Studio & exit /b 1
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo Visual Studio con il compilatore C++ non trovato & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

:build
if not exist out mkdir out
rc /nologo /fo out\res.res src\res.rc || exit /b 1

rem /MT: nessuna DLL del runtime C++ da installare; niente eccezioni ne' RTTI; ottimizzato per la dimensione
set CF=/nologo /O1 /GS- /GR- /EHs-c- /MT /W3 /DUNICODE /D_UNICODE /Foout\
set LF=/link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
set LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib wtsapi32.lib dwmapi.lib d2d1.lib dwrite.lib ntdll.lib iphlpapi.lib powrprof.lib winhttp.lib cfgmgr32.lib

cl %CF% src\main.cpp src\popups.cpp src\metrics.cpp src\slow.cpp src\sensors.cpp src\draw.cpp out\res.res /Feout\Pulse2.exe %LF% %LIBS% || exit /b 1

echo.
for %%f in (out\*.exe) do echo %%~nxf  %%~zf byte
