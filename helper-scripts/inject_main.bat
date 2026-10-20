@echo off
REM ============================================================
REM  Request administrator rights, then run the injector.
REM  (OpenProcess on the game returns err=5 without elevation)
REM ============================================================
setlocal
cd /d "%~dp0."

net session >nul 2>&1
if errorlevel 1 (
    echo [*] Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    if errorlevel 1 (
        echo.
        echo [!] Could not restart with administrator rights.
        echo     Right-click this file and choose "Run as administrator".
        echo.
        pause >nul
    )
    exit /b
)

REM ---- running elevated from here on ----------------------------------------
REM Prefer the packaged exe: players do not need Python installed at all.
REM The .py stays as the source; if there is no exe (dev machine that has not
REM built one yet), fall back to it. Whichever runs is printed, so the log
REM never leaves it ambiguous which path was taken.
REM Call the exe by full path: with NoDefaultCurrentDirectoryInExePath set
REM (some shells set it) a bare "inject_summonai.exe" is not looked up in the
REM current directory. Full path works everywhere.
if exist "inject_summonai.exe" (
    echo [*] using inject_summonai.exe
    "%~dp0inject_summonai.exe"
) else (
    where python >nul 2>&1
    if errorlevel 1 (
        echo [!] Neither inject_summonai.exe nor python was found.
        echo.
        echo     On a dev machine, build_injector.bat produces the exe.
        echo     Otherwise install Python 3 from https://www.python.org/downloads/
        echo.
        echo ---- press any key to close ----
        pause >nul
        exit /b 1
    )
    echo [*] using python inject_summonai.py
    python inject_summonai.py
)
if errorlevel 1 (
    echo.
    echo ---- the injector failed, press any key to close ----
    pause >nul
)
