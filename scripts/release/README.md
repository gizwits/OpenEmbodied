# 发布脚本使用说明

## 功能

这个脚本用于构建和发布 ESP32 固件，支持：
- 自动构建不同板型的固件
- 打包二进制文件和烧录脚本
- 自动上传到腾讯云 COS

## 安装依赖

首次使用前需要安装依赖：

```bash
cd scripts
python install_deps.py
```

或者手动安装：

```bash
pip install boto3 botocore
```

## 配置腾讯云 COS

设置以下环境变量：

```bash
export COS_SECRET_ID="你的腾讯云 SecretId"
export COS_SECRET_KEY="你的腾讯云 SecretKey"
export COS_BUCKET_NAME="你的存储桶名称"
export COS_REGION="ap-beijing"  # 可选，默认为 ap-beijing
```

## 使用方法

### 发布当前构建的固件

```bash
python release.py
```

### 发布指定板型的固件

```bash
python release.py <board_type>
```

例如：
```bash
python release.py esp-box
```

### 发布所有板型的固件

```bash
python release.py all
```

## 输出

脚本会在 `releases/` 目录下生成压缩包，包含：
- 二进制文件（bootloader.bin, partition-table.bin 等）
- 烧录脚本（flash.sh）
- 自动上传到腾讯云 COS 并显示下载链接

## 文件结构

```
releases/
├── v1.0.0_esp-box.zip
├── v1.0.0_atoms3r-echo-base.zip
└── ...
```

每个压缩包包含：
- bootloader.bin
- partition-table.bin
- srmodels.bin
- ota_data_initial.bin
- xiaozhi.bin
- xiaozhi.elf
- xiaozhi.map
- flash.sh (烧录脚本) 