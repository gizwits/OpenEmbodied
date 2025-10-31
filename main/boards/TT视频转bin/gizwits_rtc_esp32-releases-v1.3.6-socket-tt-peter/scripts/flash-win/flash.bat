@echo off
setlocal EnableDelayedExpansion

set "ScriptPath=%~dp0"
echo ScriptPath = %ScriptPath%
echo esptool.exe --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\..\..\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\..\..\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\..\..\build\ota_data_initial.bin 0x20000 %ScriptPath%\..\..\build\GizwitsRTC.bin 0x6e0000 %ScriptPath%\..\..\build\flash_tone.bin
esptool.exe --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\..\..\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\..\..\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\..\..\build\ota_data_initial.bin 0x20000 %ScriptPath%\..\..\build\GizwitsRTC.bin 0x6e0000 %ScriptPath%\..\..\build\flash_tone.bin

@REM pause