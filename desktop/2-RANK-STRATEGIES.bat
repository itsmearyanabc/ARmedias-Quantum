@echo off
rem Ranks every strategy on the downloaded history and writes the champion
rem where the EA reads it.
setlocal
cd /d "%~dp0"
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
set "RC=0"
title ARmedias Quantum - rank the strategies
echo.
echo  ARmedias Quantum: rank every strategy
echo  -------------------------------------
if exist "bin\lab.exe" goto :have_lab
echo  bin\lab.exe is missing. Use the folder from the ARmedias-Quantum download.
set "RC=1"
goto :done

:have_lab
if exist "data\ticks\XAUUSD\*.bin" goto :have_data
echo  No price history yet: run 0-DOWNLOAD-DATA.bat first.
set "RC=1"
goto :done

:have_data
set "MT5="
if exist "mt5-folder.txt" set /p MT5=<"mt5-folder.txt"
if not defined MT5 goto :rank_only
"bin\lab.exe" "data\ticks\XAUUSD" XAUUSD --out "lab" --champion-file "%MT5%\MQL5\Files\xau_champion.txt"
set "RC=%ERRORLEVEL%"
goto :ranked

:rank_only
echo  (1-INSTALL-INTO-MT5.bat has not run: the champion stays in the lab folder)
"bin\lab.exe" "data\ticks\XAUUSD" XAUUSD --out "lab"
set "RC=%ERRORLEVEL%"

:ranked
if not "%RC%"=="0" goto :done
echo.
echo  The full table: lab\leaderboard_XAUUSD.csv
if not defined QUIET start "" "lab\leaderboard_XAUUSD.csv"

:done
if not defined QUIET pause
exit /b %RC%
