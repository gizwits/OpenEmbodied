#!/bin/bash

ScriptPath=$(dirname "$(readlink -f "$0")")
echo "ScriptPath = $ScriptPath"
echo "0x0 $ScriptPath/build/bootloader/bootloader.bin 0x10000 $ScriptPath/build/partition_table/partition-table.bin 0x17000 $ScriptPath/build/ota_data_initial.bin 0x20000 $ScriptPath/build/GizwitsRTC.bin 0x660000 $ScriptPath/build/flash_tone.bin 0x800000 $ScriptPath/build/srmodels/srmodels.bin"

if [ -d "$ScriptPath/target" ]; then
    rm -rf "$ScriptPath/target"/*
else
    mkdir -p "$ScriptPath/target"
fi

echo "== copy bootloader.bin"
cp "$ScriptPath/build/bootloader/bootloader.bin" "$ScriptPath/target/"
echo "== copy partition-table.bin"
cp "$ScriptPath/build/partition_table/partition-table.bin" "$ScriptPath/target/"
echo "== copy ota_data_initial.bin"
cp "$ScriptPath/build/ota_data_initial.bin" "$ScriptPath/target/"
echo "== copy app.bin"
cp "$ScriptPath/build/GizwitsRTC.bin" "$ScriptPath/target/app.bin"
echo "== copy flash_tone.bin"
cp "$ScriptPath/build/flash_tone.bin" "$ScriptPath/target/"
echo "== copy GizwitsRTC.elf"
cp "$ScriptPath/build/GizwitsRTC.elf" "$ScriptPath/target/"
echo "== copy srmodels.bin"
cp "$ScriptPath/build/srmodels/srmodels.bin" "$ScriptPath/target/"


echo "== copy flash.bat"
echo "esptool.exe --chip esp32s3 -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x10000 partition-table.bin 0x17000 ota_data_initial.bin 0x20000 app.bin 0x660000 flash_tone.bin 0x800000 srmodels.bin" > "$ScriptPath/target/flash.bat"

if [ ! -d "$ScriptPath/bin" ]; then
    mkdir -p "$ScriptPath/bin"
    echo "Created bin directory"
fi

ARCH_NAME=websocket-TTMU0105-WS010451
echo "== rm bin/$ARCH_NAME.tar.gz"
rm -f "$ScriptPath/bin/$ARCH_NAME.tar.gz"
echo "== tar zcvf bin/$ARCH_NAME.tar.gz"
tar zcvf "$ScriptPath/bin/$ARCH_NAME.tar.gz" -C "$ScriptPath/target" bootloader.bin partition-table.bin ota_data_initial.bin app.bin flash_tone.bin flash.bat GizwitsRTC.elf srmodels.bin
echo "============ List contents of $ARCH_NAME.tar.gz ============"
tar -ztvf "$ScriptPath/bin/$ARCH_NAME.tar.gz"
