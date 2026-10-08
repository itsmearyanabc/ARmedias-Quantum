@echo off
rem Downloads gold tick history from Dukascopy into data\ticks\XAUUSD.
setlocal
cd /d "%~dp0"
set "QUIET="
if /i "%~1"=="quiet" set "QUIET=1"
set "RC=0"
title ARmedias Quantum - download gold price history
echo.
echo  ARmedias Quantum: download gold price history
echo  ---------------------------------------------
echo  Ten years of ticks is tens of thousands of downloads: allow an hour or more.
echo  What is already downloaded is kept, so running this again is quick.
echo.

rem --- find Python 3 (the Microsoft Store stub named "python" does not count)
set "PY="
where py >nul 2>&1 && py -3 -c "import sys" >nul 2>&1 && set "PY=py -3"
if defined PY goto :have_python
where python >nul 2>&1 && python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1 && set "PY=python"
if defined PY goto :have_python
echo  Python 3 is not installed.
echo  Install it from https://www.python.org/downloads/ and tick
echo  "Add python.exe to PATH" in the installer. Then run this again.
set "RC=1"
goto :done

:have_python
set "START=2015-01"
set "END="
for /f %%M in ('powershell -NoProfile -Command "(Get-Date).AddMonths(-1).ToString('yyyy-MM')"') do set "END=%%M"
if defined QUIET goto :have_dates
set /p "START=First month to download, YYYY-MM [%START%]: "
set /p "END=Last month to download, YYYY-MM [%END%]: "
:have_dates
if not defined END (
  echo  No last month given.
  set "RC=1"
  goto :done
)

echo.
echo  Installing the two Python packages it needs: numpy, requests ...
%PY% -m pip install --user --quiet --disable-pip-version-check numpy requests
if errorlevel 1 (
  echo  Installing them failed. Check the internet connection and run this again.
  set "RC=1"
  goto :done
)

echo  Downloading %START% to %END% ...
pushd python
%PY% -m xau_ingest.dukascopy --start %START% --end %END% --out "..\data\ticks\XAUUSD" --cache "..\data\raw\dukascopy" --verify
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :failed
echo.
echo  Done. Next: 2-RANK-STRATEGIES.bat
goto :done

:failed
echo.
echo  The download did not finish cleanly. Run this again: it carries on where it stopped.

:done
if not defined QUIET pause
exit /b %RC%
