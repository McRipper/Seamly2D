@echo off
setlocal
if defined SEAMLY2D_APP goto configured
if exist "%ProgramFiles%\Seamly2D\seamly2d-mcp.exe" (
    "%ProgramFiles%\Seamly2D\seamly2d-mcp.exe" %*
    exit /b
)
if exist "%ProgramFiles(x86)%\Seamly2D\seamly2d-mcp.exe" (
    "%ProgramFiles(x86)%\Seamly2D\seamly2d-mcp.exe" %*
    exit /b
)
where seamly2d-mcp.exe >nul 2>nul
if errorlevel 1 (
    echo Install Seamly2D or set SEAMLY2D_APP to its installation directory. 1>&2
    exit /b 1
)
seamly2d-mcp.exe %*
exit /b

:configured
if not exist "%SEAMLY2D_APP%\seamly2d-mcp.exe" (
    echo SEAMLY2D_APP must point to the directory containing seamly2d-mcp.exe. 1>&2
    exit /b 1
)
"%SEAMLY2D_APP%\seamly2d-mcp.exe" %*
exit /b
