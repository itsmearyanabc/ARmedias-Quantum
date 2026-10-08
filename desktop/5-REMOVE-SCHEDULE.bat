@echo off
rem Removes the daily task 4-SCHEDULE-DAILY-UPDATE.bat created.
setlocal
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
schtasks /Delete /F /TN "ARmedias Quantum live update"
set "RC=%ERRORLEVEL%"
if not defined QUIET pause
exit /b %RC%
