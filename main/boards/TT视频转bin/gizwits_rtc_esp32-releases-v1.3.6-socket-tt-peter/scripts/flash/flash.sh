#!/bin/bash
# 增加 -p 参数
if [ "$1" = "-p" ]; then
    port=$2
    shift 2
else
    port="/dev/tty.usbmodem14101"
fi

esptool.py -b 460800 -p $port --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 bootloader.bin 0x20000 app.bin 0x10000 partition-table.bin 0x17000 ota_data_initial.bin 0x800000 srmodels/srmodels.bin 0x6e0000 flash_tone.bin

python3.7 index.py -p $port