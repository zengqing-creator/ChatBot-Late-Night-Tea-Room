  @echo off
  setlocal
  chcp 65001 >nul

  set "PG_HOST=192.168.0.187"
  set "PG_PORT=5432"
  set "PG_DB=postgres"
  set "PG_USER=zengqing"
  if not defined PG_PASSWORD set "PG_PASSWORD=xxxxxxxxxxx"
  set "PG_POOL_SIZE=10"

  set "REDIS_HOST=192.168.0.61"
  set "REDIS_PORT=6379"
  set "REDIS_PASSWORD=xxxxxxxxxxx"

  if not defined DASHSCOPE_API_KEY set "DASHSCOPE_API_KEY=xxxxxxxxxxx"
  if not defined CHATBOT_API_TOKEN set "CHATBOT_API_TOKEN=xxxxxxxxxxx"

  if exist "%~dp0config.runtime.bat" call "%~dp0config.runtime.bat"

  if "%PG_PASSWORD%"=="xxxxxxxxxxx" goto :missing_credentials
  if "%REDIS_PASSWORD%"=="xxxxxxxxxxx" goto :missing_credentials
  if "%DASHSCOPE_API_KEY%"=="xxxxxxxxxxx" goto :missing_credentials
  if "%CHATBOT_API_TOKEN%"=="xxxxxxxxxxx" goto :missing_credentials

  set "CHATBOT_CA_BUNDLE=%~dp0cacert.pem"
  cd /d "%~dp0"

  if not defined ECS_IP set "ECS_IP=YOUR_ECS_PUBLIC_IP"

  echo ==============================================
  echo   ChatBot 访问地址
  echo   本地 Web : http://localhost:8080
  echo   ECS  Web : http://%ECS_IP%:8080
  echo   本地语音 : ws://localhost:8081/voice
  echo   ECS  语音 : ws://%ECS_IP%:8081/voice
  echo ==============================================
  echo.
  echo Starting ChatBot...
  ChatBot.exe
  echo.
  echo ChatBot exit code: %errorlevel%

  pause
  exit /b 0

:missing_credentials
  echo 请先填写 PG_PASSWORD、REDIS_PASSWORD、DASHSCOPE_API_KEY 和 CHATBOT_API_TOKEN。
  echo 如果 Redis 无密码，请把 REDIS_PASSWORD 设为空字符串。
  pause
  exit /b 1
