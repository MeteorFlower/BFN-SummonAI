@echo off
REM ============================================================
REM  SummonAI.dll build script (MSVC)
REM
REM  This DLL is INJECTED into the running game with a DLL
REM  injector. It does NOT hijack RtWorkQ.dll -- that slot
REM  belongs to AutoOffline, which is required to even start
REM  the game.
REM
REM  IMPORTANT: close the game before building. While it is
REM  running, SummonAI.dll is locked and the linker cannot
REM  overwrite it (LNK1104).
REM ============================================================
setlocal
cd /d "%~dp0"

REM ---- locate the Visual Studio C++ toolchain -------------------------------
REM  Preferred: vswhere.exe, which ships with every Visual Studio installer.
REM  Fallback: scan the install roots for any version / edition.
set "VCVARS="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" call :findWithVswhere

if not defined VCVARS (
    for %%R in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
        for /d %%V in ("%%~R\Microsoft Visual Studio\*") do (
            for /d %%E in ("%%~V\*") do (
                if not defined VCVARS (
                    if exist "%%~E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~E\VC\Auxiliary\Build\vcvars64.bat"
                )
            )
        )
    )
)

if not defined VCVARS (
    echo [!] Could not find vcvars64.bat.
    echo.
    echo     You need the Visual Studio C++ build tools. The free
    echo     "Build Tools for Visual Studio" is enough:
    echo       https://visualstudio.microsoft.com/downloads/
    echo     Select the "Desktop development with C++" workload.
    echo.
    echo     If Visual Studio is installed somewhere unusual, set VCVARS
    echo     manually near the top of this script.
    exit /b 1
)

echo [*] Using: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 ( echo [!] vcvars failed & exit /b 1 )

if not exist obj mkdir obj

REM Print the toolchain, for reference. A different MSVC version produces a
REM different binary - see README.md.
cl 2>&1 | findstr /C:"Version"
link 2>&1 | findstr /C:"Version"

echo [*] Compiling...
REM /Brepro = reproducible build. Without it the linker stamps the build time
REM into the binary, so no two builds of the same source would ever match.
REM MinHook source is compiled straight into the DLL -- no .lib, no extra
REM dependency. Only the four files x64 needs; hde32.c is 32-bit only.
set MINHOOK=third_party\minhook\src\hook.c ^
            third_party\minhook\src\trampoline.c ^
            third_party\minhook\src\buffer.c ^
            third_party\minhook\src\hde\hde64.c

cl /nologo /LD /O2 /MT /EHsc /W3 /Brepro /D_CRT_SECURE_NO_WARNINGS ^
   SummonAI.cpp %MINHOOK% /Fe:SummonAI.dll /Fo:obj\ ^
   /link kernel32.lib user32.lib gdi32.lib
if errorlevel 1 goto :build_failed

echo.
echo [*] Done:
dir /b SummonAI.dll
echo.
echo Inject it with inject_main.bat.
echo Log goes to summonai_log.txt next to this DLL.
exit /b 0

REM ---- ask vswhere where Visual Studio is ------------------------------------
:findWithVswhere
REM  The result is read back from a file rather than with a `for /f` backtick
REM  command: cmd's quote handling for those is fragile when the executable
REM  path contains parentheses, as "Program Files (x86)" does.
set "VSW_TMP=%TEMP%\_build_vswhere.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSW_TMP%" 2>nul
for /f "usebackq tokens=*" %%i in ("%VSW_TMP%") do (
    if not defined VCVARS if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
del "%VSW_TMP%" 2>nul
exit /b 0

REM ---- failure path ----------------------------------------------------------
:build_failed
echo.
echo [!] BUILD FAILED
echo     If it says LNK1104 "cannot open SummonAI.dll", the DLL is currently LOADED
echo     in a process -- i.e. you already injected it. Close the game and rebuild.
echo     NOTE: the game merely RUNNING does NOT lock the file. Only injection does.
echo     Other errors above are real compile errors.
exit /b 1
