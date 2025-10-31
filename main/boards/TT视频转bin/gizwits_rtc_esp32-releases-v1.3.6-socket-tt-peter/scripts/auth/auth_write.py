import os
import subprocess
import sys


def write_license_to_flash(auth_key, device_id):
    """将授权信息写入flash"""
    # 验证输入长度
    if len(auth_key) != 32:
        print("Error: auth_key must be 32 characters")
        return False

    if len(device_id) != 8:
        print("Error: device_id must be 8 characters")
        return False

    # 创建数据字符串
    data = f"{auth_key},{device_id}"

    # 计算校验和
    checksum = sum(ord(c) for c in data)

    # 创建完整的数据字符串（写入两次）
    full_data = f"{data},{checksum};{data},{checksum}"

    try:
        auth_bin = f"auth_{device_id}.bin"
        # 写入到临时文件
        with open(auth_bin, "w") as f:
            f.write(full_data)

        # 使用esptool写入flash
        cmd = ['python','D:\workspace\esptool\esptool.py', 'write_flash', '0x744000', auth_bin]
        result = subprocess.run(cmd, capture_output=True, text=True)
        os.remove(auth_bin)
        # print(result)
        if result.returncode == 0:
            # print(f"Written data: {full_data}")
            return True
        else:
            print(f"Error writing to flash: {result.stderr}", file=sys.stderr)
            return False

    except Exception as e:
        print(f"Exception while writing auth: {e}", file=sys.stderr)
        return False


if write_license_to_flash("bd85f92baa52401d93e75ef9b4ca9bbc", "fc3a0d1c"):
    print("授权信息写入成功")
else:
    print("授权信息写入失败")


# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf278
# 设备ID: f60145e5
# 设备授权码: 0990515903384adaa6cc14be8f813df6

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf09c
# 设备ID: fc25f03f
# 设备授权码: 21319563566f4b54a951eb673910f9c5

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf0ec
# 设备ID: f00695c1
# 设备授权码: 90d9064df96a4c14848d093f2a31b7a5


# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf2d0
# 设备ID: f5699625
# 设备授权码: 5d20ba6239ad4a69be3ad0b413c83987


# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf2c4
# 设备ID: f1910f3c
# 设备授权码: 59e7d52a233a4ed6a8bae942be4b1926