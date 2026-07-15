@echo off
cd /d "%~dp0"
python excel_exporter.py "..\..\Config\Excel" --output "..\..\Config\Excel"
if errorlevel 1 (
    echo.
    echo [错误] 导出过程中出现问题，请检查上方输出。
)
pause
