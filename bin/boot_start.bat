@echo off

rem change to your WDK version
set PATH=%PATH%;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64

set NAME=usbip

set LOG=HKLM\System\CurrentControlSet\Control\WMI\Autologger\%NAME%
set LOG_UDE=%LOG%-ude
set LOG_FLT=%LOG%-flt

set PROV_UDE=%LOG_UDE%\{75c66adb-e7a7-4187-8102-2eeacb97bbe5}
set PROV_FLT=%LOG_FLT%\{fa9a1b21-5ed1-4ce1-86fc-760e2d3588e4}

reg add %PROV_UDE% /f
reg add %PROV_FLT% /f

reg add %PROV_UDE% /v Enabled /t REG_DWORD /d 1 /f
reg add %PROV_FLT% /v Enabled /t REG_DWORD /d 1 /f

reg add %PROV_UDE% /v EnableLevel /t REG_DWORD /d 5 /f
reg add %PROV_FLT% /v EnableLevel /t REG_DWORD /d 5 /f

reg add %PROV_UDE% /v EnableFlags /t REG_DWORD /d 0xF /f
reg add %PROV_FLT% /v EnableFlags /t REG_DWORD /d 0x3 /f

reg add %LOG_UDE% /v Guid /t REG_SZ /d {d0a2a4e9-a7e9-4885-b8c5-152ee85ec1b4} /f
reg add %LOG_FLT% /v Guid /t REG_SZ /d {48b0b074-2836-4e53-8817-a4c9d859e98d} /f

reg add %LOG_UDE% /v Start /t REG_DWORD /d 1 /f
reg add %LOG_FLT% /v Start /t REG_DWORD /d 1 /f

tracepdb.exe -f "C:\Program Files\QSIUSB\*.pdb" -s -p %TEMP%\%NAME%

echo ##########################################
echo ### Reboot the system to start logging ###
echo ##########################################
