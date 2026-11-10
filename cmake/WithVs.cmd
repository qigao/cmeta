@echo off
setlocal
call "%VSDEVCMD_BAT%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
%*
exit /b %errorlevel%
