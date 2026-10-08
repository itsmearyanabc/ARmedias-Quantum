@echo off
rem Learns from the EA's live trades: retires probable losers, promotes the
rem proven, and rewrites the champion the EA follows. 4-SCHEDULE-DAILY-UPDATE
rem runs this every day.
setlocal
cd /d "%~dp0"
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
set "RC=0"
if not exist "lab\" mkdir "lab"
if defined QUIET >>"lab\live-update.log" echo ==== %DATE% %TIME%
if exist "bin\lab.exe" goto :have_lab
call :say bin\lab.exe is missing. Use the folder from the ARmedias-Quantum download.
set "RC=1"
goto :done

:have_lab
if exist "mt5-folder.txt" goto :have_mt5
call :say Run 1-INSTALL-INTO-MT5.bat first: it records where MetaTrader keeps its files.
set "RC=1"
goto :done

:have_mt5
set /p MT5=<"mt5-folder.txt"
set "FILES=%MT5%\MQL5\Files"
set "ARGS="
for %%J in ("%FILES%\xau_trades_*.csv") do call set ARGS=%%ARGS%% --live "%%~fJ"
if defined ARGS goto :update
call :say No live trades yet. The EA starts its journal with its first closed trade.
goto :done

:update
if defined QUIET goto :update_quiet
"bin\lab.exe" %ARGS% --symbol XAUUSD --out "lab" --champion-file "%FILES%\xau_champion.txt"
set "RC=%ERRORLEVEL%"
goto :done

:update_quiet
"bin\lab.exe" %ARGS% --symbol XAUUSD --out "lab" --champion-file "%FILES%\xau_champion.txt" >>"lab\live-update.log" 2>&1
set "RC=%ERRORLEVEL%"
goto :done

rem --- to the screen, or to the log when scheduled ---------------------------
:say
if defined QUIET (>>"lab\live-update.log" echo %*) else (echo  %*)
exit /b 0

:done
if not defined QUIET pause
exit /b %RC%
