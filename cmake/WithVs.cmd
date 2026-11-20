@echo off
setlocal
if not defined VSDEVCMD_ARCH set "VSDEVCMD_ARCH=%PROCESSOR_ARCHITECTURE%"
if /I "%VSDEVCMD_ARCH%"=="AMD64" set "VSDEVCMD_ARCH=x64"
call "%VSDEVCMD_BAT%" -arch=%VSDEVCMD_ARCH% -host_arch=%VSDEVCMD_ARCH% >nul
if errorlevel 1 exit /b %errorlevel%
%*
exit /b %errorlevel%
