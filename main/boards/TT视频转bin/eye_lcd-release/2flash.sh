# 创建target目录并复制烧录文件
export target_name=eye_lcd
export video_frames_name=all_frames.bin
echo "Creating target directory and copying flash files..."
mkdir -p target
cp build/bootloader/bootloader.bin target/
cp build/partition_table/partition-table.bin target/
# cp build/ota_data_initial.bin target/
cp build/${target_name}.bin target/
cp eye_bin/${video_frames_name} target/video_frames.bin

# 生成烧录命令文件
echo "Generating flash command file..."
echo @echo off > target/flash.bat
echo python -m esptool --chip esp32c3 -b 1152000 ^>> target/flash.bat
echo     --before default_reset --after hard_reset write_flash ^>> target/flash.bat
echo     --flash_mode dio --flash_size 16MB --flash_freq 80m ^>> target/flash.bat
echo     0x0 bootloader.bin ^>> target/flash.bat
echo     0x8000 partition-table.bin ^>> target/flash.bat
echo     0x20000 ${target_name}.bin ^>> target/flash.bat
echo     0xA0000 video_frames.bin >> target/flash.bat

echo "Flash files copied to target/ directory. Run flash.bat in target/ to flash."

cp -r ./target /mnt/e/workspace/ESP32-S3
# cd /mnt/e/workspace/ESP32-S3 && explorer.exe .