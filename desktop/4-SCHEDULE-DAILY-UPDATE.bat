@echo off
rem Runs 3-UPDATE-FROM-LIVE.bat every day at 23:30 while this user is logged on.
setlocal
cd /d "%~dp0"
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
set "RC=0"
set "TASK=ARmedias Quantum live update"
schtasks /Create /F /SC DAILY /ST 23:30 /TN "%TASK%" /TR "\"%~dp03-UPDATE-FROM-LIVE.bat\" quiet"
if errorlevel 1 goto :failed
echo.
echo  Scheduled: every day at 23:30 this PC runs 3-UPDATE-FROM-LIVE.bat.
echo  What it did each day: lab\live-update.log
echo  To stop it: 5-REMOVE-SCHEDULE.bat
goto :done

:failed
echo  Could not create the scheduled task.
set "RC=1"

:done
if not defined QUIET pause
exit /b %RC%
