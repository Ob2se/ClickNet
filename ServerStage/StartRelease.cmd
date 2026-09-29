@echo off
"%~dp0x64\Release\clicknetserver.exe" %*
if errorlevel 1 (
    echo.
    echo Server startup failed. The error is shown above.
    pause
)
