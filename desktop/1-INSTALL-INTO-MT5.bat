@echo off
rem Copies the bridge DLL and the EA into every MetaTrader 5 data folder on
rem this PC, then compiles the EA with that terminal's MetaEditor.
setlocal
cd /d "%~dp0"
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
set "RC=0"
title ARmedias Quantum - install into MetaTrader 5
echo.
echo  ARmedias Quantum: install into MetaTrader 5
echo  -------------------------------------------
if exist "bin\xaubridge.dll" goto :have_dll
echo  bin\xaubridge.dll is missing. Use the folder from the ARmedias-Quantum
echo  download of a green CI run, unzipped as it is. See START-HERE.txt.
set "RC=1"
goto :done

:have_dll
set "ROOT=%APPDATA%\MetaQuotes\Terminal"
set "COUNT=0"
set "FIRST="
if not exist "%ROOT%\" goto :none
for /d %%T in ("%ROOT%\*") do if exist "%%~T\MQL5\Experts\" call :install "%%~T"
if "%COUNT%"=="0" goto :none
>"mt5-folder.txt" echo %FIRST%
echo.
echo  The lab and the live update will use: %FIRST%
echo.
echo  Next, in MetaTrader 5:
echo    1. Tools - Options - Expert Advisors: tick "Allow DLL imports".
echo    2. Open a gold chart on a DEMO account and drag XauBridgeEA onto it.
echo    3. Inputs: InpStrategy = @champion or a strategy name, InpDryRun = true at first.
goto :done

:none
echo  No MetaTrader 5 data folder was found in:
echo    %ROOT%
echo  Start MetaTrader 5 once, close it, and run this again. A portable install
echo  keeps its data elsewhere: copy the files by hand, see mt5\README.md.
set "RC=1"
goto :done

rem --- one terminal's data folder ------------------------------------------
:install
set "T=%~1"
echo.
echo  MetaTrader data folder:
echo    %T%
if not exist "%T%\MQL5\Libraries\" mkdir "%T%\MQL5\Libraries"
if not exist "%T%\MQL5\Files\" mkdir "%T%\MQL5\Files"
copy /y "bin\xaubridge.dll" "%T%\MQL5\Libraries\xaubridge.dll" >nul
if errorlevel 1 goto :copy_failed
copy /y "mt5\XauBridgeEA.mq5" "%T%\MQL5\Experts\XauBridgeEA.mq5" >nul
if errorlevel 1 goto :copy_failed
echo    copied xaubridge.dll and XauBridgeEA.mq5
set /a COUNT+=1
if not defined FIRST set "FIRST=%T%"
call :compile
exit /b 0

:copy_failed
echo    COULD NOT copy the files: close MetaTrader 5 and run this again.
set "RC=1"
exit /b 0

rem --- compile with the MetaEditor installed beside this terminal ---------
:compile
set "ORIGIN="
if exist "%T%\origin.txt" for /f "usebackq delims=" %%O in (`type "%T%\origin.txt"`) do if not defined ORIGIN set "ORIGIN=%%O"
set "ME="
if defined ORIGIN if exist "%ORIGIN%\metaeditor64.exe" set "ME=%ORIGIN%\metaeditor64.exe"
if defined ME goto :have_me
echo    MetaEditor not found for this terminal: open MetaEditor, open
echo    XauBridgeEA.mq5 and press F7 to compile it.
exit /b 0

:have_me
set "LOG=%T%\MQL5\Experts\XauBridgeEA.compile.log"
if exist "%LOG%" del "%LOG%"
echo    compiling with MetaEditor ...
start "" /wait "%ME%" /compile:"%T%\MQL5\Experts\XauBridgeEA.mq5" /inc:"%T%\MQL5" /log:"%LOG%"
if exist "%LOG%" goto :have_log
echo    MetaEditor wrote no log: open the EA in MetaEditor and press F7.
exit /b 0

:have_log
type "%LOG%" | findstr /i /c:"result" /c:"error" /c:"warning"
type "%LOG%" | findstr /i /r /c:"[^0-9]0 errors" /c:"^0 errors" >nul
if errorlevel 1 goto :compile_failed
echo    compiled: XauBridgeEA is ready in the Navigator under Expert Advisors.
exit /b 0

:compile_failed
echo    THE EA DID NOT COMPILE. Send the lines above to Claude to get it fixed.
echo    The full log: %LOG%
set "RC=1"
exit /b 0

:done
if not defined QUIET pause
exit /b %RC%
