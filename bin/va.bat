rem change to your WDK version
set PATH=%PATH%;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64

set NAME=usbip

tracelog.exe -stop %NAME%-flt
tracelog.exe -stop %NAME%-ude

del /F %NAME%-*.*
tracepdb.exe -f "..\x64\Debug\*.pdb" -s -p %TEMP%\%NAME%

tracelog.exe -start %NAME%-flt -guid #fa9a1b21-5ed1-4ce1-86fc-760e2d3588e4 -f %NAME%-flt.etl -flag 0x3 -level 5
tracelog.exe -start %NAME%-ude -guid #75c66adb-e7a7-4187-8102-2eeacb97bbe5 -f %NAME%-ude.etl -flag 0xF -level 5
