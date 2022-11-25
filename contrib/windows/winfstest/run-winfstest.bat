@echo off

set PythonRegKey=HKLM\SOFTWARE\Python\PythonCore\2.7
reg query %PythonRegKey%\InstallPath /ve >nul 2>&1 || (echo Cannot find Windows Python >&2 & exit /b 1)
for /f "tokens=2,*" %%i in ('reg query %PythonRegKey%\InstallPath /ve ^| findstr Default') do (
    set PythonInstallPath=%%j
)

set PYTHONPATH=%~dp0
"%PythonInstallPath%\python.exe" %~dp0simpletap.py --run %*
