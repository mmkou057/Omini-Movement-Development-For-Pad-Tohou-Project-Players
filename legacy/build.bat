@echo off
rem Legacy build: external SendInput injector (th15_injector.exe).
rem Superseded by the dinput8.dll proxy approach (double-input / frame-sync
rem issues). Kept for history only. Requires .NET Framework csc, x86.
setlocal
set "SRC=%~dp0"
set "CSC=%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo [error] csc.exe not found: %CSC%
    exit /b 1
)
cd /d "%SRC%"
%CSC% /nologo /platform:x86 /target:exe /out:th15_injector.exe /optimize+ Program.cs
if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)
echo [ok] th15_injector.exe built
endlocal
