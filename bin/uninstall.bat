@echo off

SET HWID=ROOT\QSIUSB\UDE
set APPDIR=C:\Program Files\QSIUSB

"%APPDIR%\devnode.exe" remove %HWID% root
rem alternative command since Windows 11, version 21H2
rem pnputil.exe /remove-device /deviceid %HWID% /subtree

rem WARNING: use %%P and %%~nxP if you run this command in a .bat file
FOR /f %%P IN ('findstr /M /L /Q:u "qsiusb_filter qsiusb_ude" C:\WINDOWS\INF\oem*.inf') DO pnputil.exe /delete-driver %%~nxP /uninstall

rd /S /Q "%APPDIR%"
