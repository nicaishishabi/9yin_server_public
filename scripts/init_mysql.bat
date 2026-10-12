@echo off
setlocal
cd /d "%~dp0.."
set MYSQL_BIN=
if exist "mysql-server\mysql-26.7.0-winx64\bin\mysql.exe" set MYSQL_BIN=mysql-server\mysql-26.7.0-winx64\bin\mysql.exe
if "%MYSQL_BIN%"=="" (
  where mysql >nul 2>&1 && set MYSQL_BIN=mysql
)
if "%MYSQL_BIN%"=="" (
  echo [ERROR] mysql.exe not found. Start project MySQL or add mysql to PATH.
  exit /b 1
)
echo Using: %MYSQL_BIN%
echo Applying migrations\0001_init_roles.sql ...
"%MYSQL_BIN%" -u nineyin -pnineyin123 < migrations\0001_init_roles.sql 2>nul
if errorlevel 1 (
  echo Trying as root...
  "%MYSQL_BIN%" -u root < migrations\0001_init_roles.sql
)
echo Done. Create mysql.env from mysql.env.example if needed.
endlocal
