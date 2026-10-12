@echo off
setlocal EnableExtensions
title Nine Yin MySQL Setup
cd /d "%~dp0"

echo ============================================
echo  Nine Yin 数据库初始化 (MySQL)
echo ============================================

REM ---- 1. locate the mysql client ----
set "MYSQL="
REM 优先使用项目内自带的 MySQL 客户端
if exist "%~dp0mysql-server\mysql-26.7.0-winx64\bin\mysql.exe" (
  set "MYSQL=%~dp0mysql-server\mysql-26.7.0-winx64\bin\mysql.exe"
)
if not defined MYSQL (
  where mysql >nul 2>&1
  if not errorlevel 1 (
    set "MYSQL=mysql"
  ) else (
    for %%P in (
      "%PROGRAMFILES%\MySQL\MySQL Server 8.0\bin\mysql.exe"
      "%PROGRAMFILES%\MySQL\MySQL Server 8.4\bin\mysql.exe"
      "%PROGRAMFILES%\MySQL\MySQL Server 9.0\bin\mysql.exe"
      "%PROGRAMFILES(x86)%\MySQL\MySQL Server 8.0\bin\mysql.exe"
    ) do (
      if exist "%%~P" set "MYSQL=%%~P"
    )
  )
)
if not defined MYSQL (
  echo ERROR: 未找到 mysql 客户端。
  echo 请先安装 MySQL, 例如:
  echo   winget install Oracle.MySQL
  echo 或下载 MySQL Community ZIP, 解压后把 bin 目录加入 PATH。
  pause
  exit /b 1
)
echo 使用 mysql 客户端: %MYSQL%

REM ---- 2. root 连接参数 ----
set "ROOTOPTS="
set /p ROOTPASS=请输入 MySQL root 密码 (无密码直接回车):
if not "%ROOTPASS%"=="" set "ROOTOPTS=--password=%ROOTPASS%"

set "DBNAME=nineyin"
set "DBUSER=nineyin"
set /p DBPASS=请为新建用户 %DBUSER% 设置密码:

REM ---- 3. create database and user ----
"%MYSQL%" -u root %ROOTOPTS% -e "CREATE DATABASE IF NOT EXISTS %DBNAME% DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci; CREATE USER IF NOT EXISTS '%DBUSER%'@'localhost' IDENTIFIED BY '%DBPASS%'; GRANT ALL PRIVILEGES ON %DBNAME%.* TO '%DBUSER%'@'localhost'; FLUSH PRIVILEGES;"
if errorlevel 1 (
  echo ERROR: 创建数据库/用户失败。请检查 root 密码是否正确。
  pause
  exit /b 1
)
echo 数据库 %DBNAME% 与用户 %DBUSER% 已就绪。

REM ---- 4. write mysql.env (used by 启动本地九阴服务.bat) ----
set "DSN=%DBUSER%:%DBPASS%@tcp(127.0.0.1:3306)/%DBNAME%"
> "%~dp0mysql.env" echo set "NINEYIN_MYSQL_DSN=%DSN%"
echo mysql.env 已生成 (内含密码, 请勿提交到版本库)。

REM ---- 5. apply schema migrations and import existing JSON data ----
set "NINEYIN_MYSQL_DSN=%DSN%"
where go >nul 2>&1
if errorlevel 1 (
  echo WARNING: 未找到 Go, 跳过建表与数据导入。请安装 Go 后手动运行:
  echo   go run ./cmd/role-migrate
  echo   go run ./cmd/data-migrate
  pause
  exit /b 0
)

echo.
echo 应用数据库迁移 (role-migrate)...
go run ./cmd/role-migrate
if errorlevel 1 (
  echo ERROR: 迁移失败。
  pause
  exit /b 1
)

echo.
echo 导入现有 data\*.json 到 MySQL (data-migrate)...
go run ./cmd/data-migrate
if errorlevel 1 (
  echo ERROR: 数据导入失败。
  pause
  exit /b 1
)

echo.
echo ============================================
echo  完成! 现在运行 启动本地九阴服务.bat 即可使用 MySQL。
echo ============================================
pause
