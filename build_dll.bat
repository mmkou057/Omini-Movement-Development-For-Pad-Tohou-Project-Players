@echo off
REM build_dll.bat - Build dinput8.dll (32-bit DLL proxy) with i686 MinGW-w64 gcc
REM
REM Toolchain: WinLibs i686. Either put gcc on PATH, or unzip the WinLibs
REM            i686 archive so that ..\mingw32\bin\gcc.exe exists next to
REM            this repository folder. You can also override with set GCC=...
REM
REM Output: dinput8.dll - copy it next to th*.exe (no injector needed).
setlocal

set "SRC=%~dp0"
if not defined GCC set "GCC=%SRC%..\mingw32\bin\gcc.exe"
if not exist "%GCC%" set "GCC=gcc"

cd /d "%SRC%"

echo ==^> Building dinput8.dll (32-bit, padhook.c + pwm.c + vector.c)
"%GCC%" -m32 -shared -O2 -Wall ^
    -o dinput8.dll ^
    padhook.c pwm.c vector.c ^
    padhook.def ^
    -Wl,--enable-stdcall-fixup ^
    -Wl,--kill-at ^
    -lkernel32 -luser32 -lgdi32 -lwinmm -lm

if errorlevel 1 (
  echo [ERR] build failed
  exit /b 1
)

echo ==^> Verifying exported symbol
if exist "%SRC%..\mingw32\bin\objdump.exe" (
    "%SRC%..\mingw32\bin\objdump.exe" -p dinput8.dll | findstr /R "DirectInput8Create"
) else (
    objdump -p dinput8.dll | findstr /R "DirectInput8Create"
)

echo ==^> Done. Copy dinput8.dll next to th*.exe.
endlocal
