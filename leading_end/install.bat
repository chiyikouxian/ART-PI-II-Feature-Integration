@echo off
chcp 65001 >nul
echo ========================================
echo   手套数据采集系统 - 依赖安装
echo ========================================
echo.

REM 检查Python是否安装
python --version >nul 2>&1
if errorlevel 1 (
    echo [错误] 未找到Python，请先安装Python3
    pause
    exit /b 1
)

echo [信息] Python版本:
python --version

echo.
echo [信息] 创建虚拟环境...
python -m venv venv

echo [信息] 激活虚拟环境...
call venv\Scripts\activate.bat

echo [信息] 升级pip...
python -m pip install --upgrade pip -q

echo [信息] 安装项目依赖...
pip install -r requirements.txt

echo.
echo ========================================
echo   依赖安装完成！
echo   运行 start.bat 启动应用
echo ========================================
pause
