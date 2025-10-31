@REM @echo off
setlocal EnableDelayedExpansion

set "DumpPath=%~dp0dump"
set "FileName=dump_%date:~0,4%%date:~5,2%%date:~8,2%_%time:~0,2%%time:~3,2%%time:~6,2%"
set "FileName=%FileName: =0%"
echo DumpPath = %DumpPath%
echo FileName = %FileName%

if not exist "%DumpPath%" (
    mkdir "%DumpPath%"
)

@REM @echo on

set "ReadAddress=0xd00000"
set "ReadSize=0x200000"
@REM set "ReadSize=0x100000"

echo xxd -g2 -c32 dump/%FileName%.bin ^> dump/%FileName%.txt
echo esptool.exe --chip esp32s3 -b 921600 read_flash %ReadAddress% %ReadSize% "%DumpPath%/%FileName%.bin"

esptool.exe --chip esp32s3 -b 921600 --after no_reset read_flash %ReadAddress% %ReadSize% "%DumpPath%/%FileName%.bin"

@REM pause