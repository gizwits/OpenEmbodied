@echo off
set "CURRENT_PATH=%~dp0"
echo Current Path: %CURRENT_PATH%
esptool.exe --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 %CURRENT_PATH%build\bootloader\bootloader.bin 0x8000 %CURRENT_PATH%build\partition_table\partition-table.bin 0xd000 %CURRENT_PATH%build\ota_data_initial.bin 0x10000 %CURRENT_PATH%build\srmodels\srmodels.bin 0x110000 %CURRENT_PATH%build\xiaozhi.bin