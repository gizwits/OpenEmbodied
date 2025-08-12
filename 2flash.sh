# 创建target目录并复制烧录文件
echo "Creating target directory and copying flash files..."
mkdir -p target
cp build/bootloader/bootloader.bin target/
cp build/partition_table/partition-table.bin target/
cp build/ota_data_initial.bin target/
cp build/xiaozhi.bin target/
# cp build/srmodels/srmodels.bin target/

# 生成烧录命令文件
echo "Generating flash command file..."
echo @echo off > target/flash.bat
echo python -m esptool --chip esp32c2 -b 1152000 ^>> target/flash.bat
echo     --before default_reset --after hard_reset write_flash ^>> target/flash.bat
echo     --flash_mode dio --flash_size 16MB --flash_freq 80m ^>> target/flash.bat
echo     0x0 bootloader.bin ^>> target/flash.bat
echo     0x8000 partition-table.bin ^>> target/flash.bat
echo     0xd000 ota_data_initial.bin ^>> target/flash.bat
echo     0x10000 xiaozhi.bin ^>> target/flash.bat

echo "Flash files copied to target/ directory. Run flash.bat in target/ to flash."

cp -r ./target /mnt/e/workspace/ESP32-C2
# cd /mnt/e/workspace/ESP32-S3 && explorer.exe .