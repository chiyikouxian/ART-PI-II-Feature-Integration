@echo off
chcp 65001 >nul
echo ========================================
echo   手套数据采集系统 - 启动脚本
echo ========================================
echo.

REM 检查Python是否安装
python --version >nul 2>&1
if errorlevel 1 (
    echo [错误] 未找到Python，请先安装Python3
    pause
    exit /b 1
)

REM 检查是否需要安装依赖
if not exist "venv" (
    echo [信息] 创建虚拟环境...
    python -m venv venv
)

echo [信息] 激活虚拟环境...
call venv\Scripts\activate.bat

echo [信息] 安装依赖...
pip install -r requirements.txt -q

echo.
echo [信息] 启动应用...
echo [信息] 访问地址: http://127.0.0.1:5000
echo [信息] 按 Ctrl+C 停止服务
echo.

python run.py

pause
