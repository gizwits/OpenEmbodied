## TODO

## eye_lcd git
git@gitlab.gizwits.com:gizwits-gagent/eye_lcd.git

## Development
git clone https://github.com/gizwits/esp-adf
cd esp-adf
git submodule update --init --recursive

vim ~/.bash_profile

```
export IDF_PATH=/Users/Kylewang/Project/gizwits-esp-adf/esp-idf
export ADF_PATH=/Users/Kylewang/Project/gizwits-esp-adf
source ~/.bash_profile
```


```
idf.py set-target esp32s3
idf.py menuconfig
idf.py flash
idf.py monitor
```

## 烧录
cd scripts/flash
把
app.bin
bootloader.bin
ota_data_initial.bin
partition-table.bin
拷贝到当前目录（去git的tag下载）

```
sh ./flash.sh
```

## 授权
cd /scripts/auth_data
```
python3 index.py
```
