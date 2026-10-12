@echo off
setlocal EnableExtensions
title Nine Yin MySQL Server

REM ��ѡ���� %1 == --silent : ��Ĭ���� (�������ű��Զ�����, ����ͣ)
set "SILENT=0"
if /i "%1"=="--silent" set "SILENT=1"

REM MySQL ������ĿǨ�Ƶ� mysql-server\ ��, ʹ����Ŀ���·�� (%~dp0 = ���ű�����Ŀ¼)
set "MYSQL_ROOT=%~dp0mysql-server\mysql-26.7.0-winx64"
set "MYSQLD=%MYSQL_ROOT%\bin\mysqld.exe"
set "CONF=%MYSQL_ROOT%\my.ini"

if not exist "%MYSQLD%" (
  echo ERROR: mysqld.exe not found:
  echo   %MYSQLD%
  if "%SILENT%"=="0" pause
  exit /b 1
)

REM ��������ֱ���˳�
powershell.exe -NoProfile -Command "if (Get-NetTCPConnection -State Listen -LocalPort 3306 -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }"
if not errorlevel 1 (
  if "%SILENT%"=="0" echo MySQL is already listening on 3306.
  if "%SILENT%"=="0" pause
  exit /b 0
)

echo Starting MySQL 26.7.0 (port 3306)...
start "Nine Yin MySQL" /D "%MYSQL_ROOT%" "%MYSQLD%" --defaults-file="%CONF%"

REM �ȴ� mysqld ���� (�� PowerShell ˯��, ������ timeout �Ľ�������)
powershell.exe -NoProfile -Command "Start-Sleep -Seconds 6"
powershell.exe -NoProfile -Command "if (Get-NetTCPConnection -State Listen -LocalPort 3306 -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }"
if errorlevel 1 (
  echo ERROR: MySQL did not start. Check:
  echo   %MYSQL_ROOT%\data\mysql-err.log
  if "%SILENT%"=="0" pause
  exit /b 1
)
if "%SILENT%"=="0" (
  echo MySQL is listening on 127.0.0.1:3306.
  echo Now run �������ؾ�������.bat to start the game server.
  pause
)
exit /b 0
