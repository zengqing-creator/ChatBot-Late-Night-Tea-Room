@echo off
setlocal
chcp 65001 >nul

REM Public defaults; copy config.runtime.example.bat to config.runtime.bat for private settings.
if not defined PG_HOST set "PG_HOST=127.0.0.1"
if not defined PG_PORT set "PG_PORT=5432"
if not defined PG_DB set "PG_DB=postgres"
if not defined PG_USER set "PG_USER=postgres"
if not defined REDIS_HOST set "REDIS_HOST=127.0.0.1"
if not defined REDIS_PORT set "REDIS_PORT=6379"
if not defined PG_PASSWORD set "PG_PASSWORD=xxxxxxxxxxx"
if not defined DASHSCOPE_API_KEY set "DASHSCOPE_API_KEY=xxxxxxxxxxx"
if not defined CHATBOT_API_TOKEN set "CHATBOT_API_TOKEN=xxxxxxxxxxx"
if exist "%~dp0config.runtime.bat" call "%~dp0config.runtime.bat"
if "%PG_PASSWORD%"=="xxxxxxxxxxx" goto :missing_credentials
if "%DASHSCOPE_API_KEY%"=="xxxxxxxxxxx" goto :missing_credentials
if "%CHATBOT_API_TOKEN%"=="xxxxxxxxxxx" goto :missing_credentials

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"
set "EXE_DIR=%BUILD_DIR%\bin\Release"
set "EXE_NAME=ChatBot.exe"

if not exist "%PROJECT_DIR%\config.local.bat" goto :missing_build_config
call "%PROJECT_DIR%\config.local.bat"
if not defined VCPKG_ROOT goto :missing_vcpkg
if not defined PostgreSQL_ROOT goto :missing_postgresql
if not defined OPENSSL_ROOT_DIR goto :missing_openssl
set "CMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
if not exist "%CMAKE_TOOLCHAIN_FILE%" goto :missing_vcpkg
if not exist "%VCPKG_ROOT%\installed\x64-windows\share\hiredis\hiredis-config.cmake" goto :missing_hiredis
if not exist "%PostgreSQL_ROOT%\include\libpq-fe.h" goto :missing_postgresql
if not exist "%PostgreSQL_ROOT%\lib\libpq.lib" goto :missing_postgresql
if not exist "%OPENSSL_ROOT_DIR%\include\openssl\ssl.h" goto :missing_openssl
if not exist "%OPENSSL_ROOT_DIR%\lib\VC\x64\MD\libssl.lib" goto :missing_openssl
if not exist "%OPENSSL_ROOT_DIR%\lib\VC\x64\MD\libcrypto.lib" goto :missing_openssl

cd /d "%PROJECT_DIR%"

if errorlevel 1 (
    echo "Failed to change directory to %PROJECT_DIR%"
    pause
    exit /b 1
)
if not exist "%BUILD_DIR%" (
    echo "Build directory %BUILD_DIR% not found. Creating it..."
    mkdir "%BUILD_DIR%"
    if errorlevel 1 (
        echo "Failed to create build directory %BUILD_DIR%"
        pause
        exit /b 1
    )
)
echo "Configuring CMake build files..."
cmake --fresh -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "-DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN_FILE%" "-DPostgreSQL_ROOT=%PostgreSQL_ROOT%" "-DOPENSSL_ROOT_DIR=%OPENSSL_ROOT_DIR%"
if errorlevel 1 (
    echo "CMake configuration failed."
    pause
    exit /b 1
)

echo "1. Building %EXE_NAME%..."
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo "Build failed."
    pause
    exit /b 1
)

echo "2. Syncing memes/ into build output..."
if not exist "%EXE_DIR%\memes" mkdir "%EXE_DIR%\memes"
xcopy /E /Y /I "%PROJECT_DIR%\memes\*" "%EXE_DIR%\memes\" >nul

if not defined ECS_IP set "ECS_IP=YOUR_ECS_PUBLIC_IP"

echo ==============================================
echo   ChatBot 访问地址
echo   本地 Web : http://localhost:8080
echo   ECS  Web : http://%ECS_IP%:8080
echo   本地语音 : ws://localhost:8081/voice
echo   ECS  语音 : ws://%ECS_IP%:8081/voice
echo ==============================================
echo.
echo "3. Starting ChatBot..."
cd /d "%EXE_DIR%"
if not exist "%EXE_NAME%" (
    echo "%EXE_NAME% not found in %EXE_DIR%"
    pause
    exit /b 1
)
start "" "%EXE_NAME%"

echo "4. Auto-seeding memes (POST /api/memes)..."
where python >nul 2>nul
if errorlevel 1 (
    echo "python not found on PATH; skip seeding."
) else (
    python "%PROJECT_DIR%\scripts\seed_memes.py" http://localhost:8080 "%EXE_DIR%\memes"
    if errorlevel 1 echo "Seeding failed or server unreachable."
)

pause
exit /b 0

:missing_credentials
echo 请先填写 PG_PASSWORD、DASHSCOPE_API_KEY 和 CHATBOT_API_TOKEN，或在启动前设置同名环境变量。
pause
exit /b 1

:missing_build_config
echo "Missing %PROJECT_DIR%\config.local.bat. Copy config.local.example.bat and adjust the paths."
pause
exit /b 1

:missing_vcpkg
echo "Missing vcpkg toolchain: %CMAKE_TOOLCHAIN_FILE%"
pause
exit /b 1

:missing_hiredis
echo "Missing x64-windows hiredis package under %VCPKG_ROOT%."
pause
exit /b 1

:missing_postgresql
echo "Missing PostgreSQL headers or libpq.lib under %PostgreSQL_ROOT%."
pause
exit /b 1

:missing_openssl
echo "Missing OpenSSL headers or x64 MD libraries under %OPENSSL_ROOT_DIR%."
pause
exit /b 1
