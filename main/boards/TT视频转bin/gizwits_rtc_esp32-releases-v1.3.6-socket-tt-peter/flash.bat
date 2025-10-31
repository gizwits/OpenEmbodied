@echo off
setlocal EnableDelayedExpansion

set "ScriptPath=%~dp0"
echo ScriptPath = %ScriptPath%
@REM echo esptool.exe --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\build\ota_data_initial.bin 0x20000 %ScriptPath%\build\GizwitsRTC.bin 0xC00000 %ScriptPath%\build\flash_tone2.bin 0x800000 %ScriptPath%\build/srmodels/srmodels.bin
@REM esptool.exe --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\build\ota_data_initial.bin 0x20000 %ScriptPath%\build\GizwitsRTC.bin 0xC00000 %ScriptPath%\build\flash_tone2.bin 0x800000 %ScriptPath%\build/srmodels/srmodels.bin
echo esptool.exe --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\build\ota_data_initial.bin 0x20000 %ScriptPath%\build\GizwitsRTC.bin 0x660000 %ScriptPath%\build\flash_tone.bin 0x800000 %ScriptPath%\build/srmodels/srmodels.bin
esptool.exe --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %ScriptPath%\build\bootloader\bootloader.bin 0x10000 %ScriptPath%\build\partition_table\partition-table.bin 0x17000 %ScriptPath%\build\ota_data_initial.bin 0x20000 %ScriptPath%\build\GizwitsRTC.bin 0x660000 %ScriptPath%\build\flash_tone.bin 0x800000 %ScriptPath%\build/srmodels/srmodels.bin

@REM pause