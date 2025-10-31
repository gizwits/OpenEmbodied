@echo off
setlocal EnableDelayedExpansion

if "%1"=="" (
    set "COM_COUNT=0"
    for /f "tokens=*" %%a in ('powershell -command "[System.IO.Ports.SerialPort]::GetPortNames()"') do (
        set /a "COM_COUNT+=1"
        set "COM_PORT!COM_COUNT!=%%a"
    )
    
    if !COM_COUNT!==0 (
        echo No COM port found
        exit /b 1
    )
    
    if !COM_COUNT!==1 (
        set "COM_PORT=!COM_PORT1!"
        echo Using COM port: !COM_PORT!
    ) else (
        echo Available COM ports:
        for /l %%i in (1,1,!COM_COUNT!) do (
            echo %%i. !COM_PORT%%i!
        )
        set /p "SELECT=Please select COM port number (1-!COM_COUNT!): "
        set "COM_PORT=!COM_PORT%SELECT%!"
        echo Using COM port: !COM_PORT!
    )
) else (
    set "COM_PORT=%1"
)

:connect
@REM echo Connecting to !COM_PORT!...
@REM esptool.exe -p !COM_PORT! -b 921600 read_mac
@REM if !ERRORLEVEL! neq 0 (
@REM     echo Connection failed, retrying in 300ms...
@REM     timeout /t 1 /nobreak >nul
@REM     ping -n 2 127.0.0.1 >nul
@REM     goto connect
@REM )

:monitor
plink.exe -serial !COM_PORT! -sercfg 921600,8,n,1,N
if !ERRORLEVEL! neq 0 (
    echo Monitor disconnected, reconnecting in 300ms...
    timeout /t 1 /nobreak >nul
    ping -n 2 127.0.0.1 >nul
    goto connect
)
