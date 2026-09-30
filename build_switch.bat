@echo off
REM build_switch.bat - Build PadSwitch.exe (WinForms, C# 5, x86)
REM                   using the csc.exe shipped with .NET Framework
setlocal

set "SRC=%~dp0"
set "CSC=%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"

if not exist "%CSC%" (
  echo [ERR] csc not found: %CSC%
  exit /b 1
)

cd /d "%SRC%"

echo ==^> Building PadSwitch.exe (WinForms, C# 5, x86)
"%CSC%" /nologo /platform:x86 /target:winexe /utf8output ^
    /out:PadSwitch.exe ^
    /r:System.Windows.Forms.dll ^
    /r:System.Drawing.dll ^
    PadSwitch.cs

if errorlevel 1 (
  echo [ERR] build failed
  exit /b 1
)

echo ==^> Done. Run PadSwitch.exe (keep it next to dinput8.dll).
endlocal
