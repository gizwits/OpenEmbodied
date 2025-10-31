#!/usr/bin/env python3

import os
import sys
import subprocess
import requests
import json
import time
import argparse

# 定义绿色文本的 ANSI 转义序列
RED = '\033[91m'
GREEN = '\033[92m'
# 定义重置颜色的 ANSI 转义序列
RESET = '\033[0m'
SUCCESS_ASCII_ART = f"""{GREEN}
*************
*           *
*    ^_^    *
*           *
* 烧录成功啦! *
*************{RESET}"""

AUTH_ADDRESS_HOST = "http://43.139.79.34:31647"
AUTH_PRODUCT_KEY = "8179cb7ac34649fe9eaa735892aed562"  # 产品密钥
AUTH_PRODUCT_SECRET = "57125a1eee4c484ebbd806406201dc5a"  # 产品密钥
AUTH_ORG_ID = 16443


def check_device_license(device_mac, device_id, license_key):
    url = f"{AUTH_ADDRESS_HOST}/v4/products/{AUTH_PRODUCT_KEY}/licenses/{license_key}?product_secret={AUTH_PRODUCT_SECRET}&device_mac={device_mac}&device_id={device_id}"
    headers = {
        'X-Org-Id': f'{AUTH_ORG_ID}',
        'Content-Type': 'application/json'
    }
    response = requests.request("GET", url, headers=headers)
    return response


def report_org_license(mac):
    url = f"{AUTH_ADDRESS_HOST}/v4/organizations/{AUTH_ORG_ID}/licenses"
    data = {
        'device_mac': mac
    }
    response = requests.post(url, data=data, timeout=5)
    return response


def allocate_org_license(mac):
    url = f"{AUTH_ADDRESS_HOST}/v4/organizations/{AUTH_ORG_ID}/licenses"
    headers = {
        'Content-Type': 'application/json'
    }
    payload = json.dumps({
        "product_key": AUTH_PRODUCT_KEY,
        "product_secret": AUTH_PRODUCT_SECRET,
        "device_macs": [
            mac
        ]
    })
    response = requests.request("PUT", url, headers=headers, data=payload, timeout=5)
    return response


def apply_product_license(mac):
    url = f"{AUTH_ADDRESS_HOST}/v4/products/{AUTH_PRODUCT_KEY}/licenses"
    headers = {
        'X-Org-Id': f'{AUTH_ORG_ID}',
        'Content-Type': 'application/json'
    }
    payload = json.dumps({
        "product_secret": AUTH_PRODUCT_SECRET,
        "device_macs": [
            mac
        ]
    })
    response = requests.request("POST", url, headers=headers, data=payload, timeout=5)
    return response


def confirm_product_license(device_mac, device_id, license_key, state):
    url = f"{AUTH_ADDRESS_HOST}/v4/products/{AUTH_PRODUCT_KEY}/licenses/{license_key}"
    payload = json.dumps({
        "product_secret": AUTH_PRODUCT_SECRET,
        "device_id": f"{device_id}",
        "device_mac": f"{device_mac}",
        "state": state
    })
    headers = {
        'X-Org-Id': f'{AUTH_ORG_ID}',
        'Content-Type': 'application/json'
    }
    response = requests.request("PUT", url, headers=headers, data=payload, timeout=5)
    return response


# 脚本所在目录
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def get_esp32_mac(port):
    """获取设备 MAC 地址"""
    try:
        # 使用esptool.exe获取MAC地址
        cmd = ['./esptool.exe', '-p', port, 'read_mac']
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode == 0:
            # esptool输出的MAC地址通常在最后一行
            output = result.stdout.strip()
            # 查找MAC地址格式的行
            for line in output.split('\n'):
                if 'MAC:' in line:
                    mac = line.split('MAC:')[1].strip()
                    # 移除所有空格和冒号，转换为小写
                    mac = mac.replace(':', '').replace(' ', '').lower()
                    # print(f"Got MAC address: {mac}")
                    return mac
            print("MAC address not found in output", file=sys.stderr)
            return None
        else:
            print(f"Error getting MAC: {result.stderr}", file=sys.stderr)
            return None
    except Exception as e:
        print(f"Exception while getting MAC: {e}", file=sys.stderr)
        return None


def read_license_from_flash(port, device_mac):
    """从 Flash 读取授权信息"""
    try:
        auth_bin = f"auth_{device_mac}.bin"
        # 使用 esptool.exe 读取 Flash
        cmd = ['./esptool.exe', '-p', port, 'read_flash', '0x744000', '0x80', auth_bin]
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode == 0:
            # 读取文件内容
            device_id, license_key = None, None
            with open(auth_bin, 'rb') as f:
                data = f.read()
                decoded_data = data.decode('utf-8', errors='ignore').replace('\xff', '')
            os.remove(auth_bin)
            parts = decoded_data.split(',')
            if len(parts) >= 2:
                device_id, license_key = parts[1], parts[0]
            return device_id, license_key
        else:
            print(f"Error reading from flash: {result.stderr}", file=sys.stderr)
            return None, None
    except Exception as e:
        print(f"Exception while reading auth: {e}", file=sys.stderr)
        return None, None


def write_license_to_flash(port, auth_key, device_id):
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
        cmd = ['./esptool.exe', '-p', port, 'write_flash', '0x744000', auth_bin]
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


def main():
    parser = argparse.ArgumentParser(description='Device authorization tool')
    parser.add_argument('-p', '--port', required=True, help='Serial port for the device')
    args = parser.parse_args()

    # Step 1: Get MAC address of the esp32.
    device_mac = get_esp32_mac(args.port)
    if not device_mac:
        print(f"{RED}获取设备MAC失败！{RESET}")
        sys.exit(1)
    print(f"{GREEN}当前设备MAC: {device_mac}{RESET}")

    # Step 2: Check license in flash.
    device_id, license_key = read_license_from_flash(args.port, device_mac)
    if device_id:
        response = check_device_license(device_mac, device_id, license_key)
        if response.status_code == 200:
            print(f"{GREEN}烧录授权信息成功{RESET}")
            return

    # Step 3: Report and allocate license
    response = report_org_license(device_mac)
    if response.status_code != 200:
        print(f"{RED}报备设备MAC失败！错误结果：{response.text}{RESET}")
        sys.exit(1)
    print(f"{GREEN}报备设备MAC成功{RESET}")
    response = allocate_org_license(device_mac)
    if response.status_code != 200:
        print(f"{RED}分配授权许可失败！错误结果：{response.text}{RESET}")
        sys.exit(1)
    if response.json()['data'].get('failure' or 0) == 1:
        print(f"{RED}分配授权许可失败！尝试申请授权许可......{RESET}")
    else:
        print(f"{GREEN}分配授权许可成功{RESET}")

    # Step 4: Apply product license
    response = apply_product_license(device_mac)
    retry = 0
    while response.status_code != 200 and retry < 32:
        time.sleep(0.3)
        retry += 1
        print(f"{RED}申请授权许可重试中......{retry}次{RESET}")
        response = apply_product_license(device_mac)
    if response.status_code != 200 or not response.json().get('data'):
        print(f"{RED}申请授权许可失败！错误信息{response.text}，请联系管理员{RESET}")
        sys.exit(1)
    print(f"{GREEN}申请授权许可成功{RESET}")

    # Step 5: Write license to flash
    print(f"{GREEN}开始烧录授权信息......{RESET}")
    # 直接调用写入flash的函数
    license_data = response.json().get('data')[0]
    result = write_license_to_flash(args.port, license_data["license_key"], license_data["device_id"])
    if not result:
        print(f"{RED}烧录授权信息失败{RESET}")
        # 释放授权许可
        confirm_product_license(device_mac, license_data['device_id'], license_data['license_key'], 0)
        sys.exit(1)
    response = confirm_product_license(device_mac, license_data['device_id'], license_data['license_key'], 3)
    if response.status_code != 200:
        print(f"{RED}确认云端设备MAC失败！请联系管理员{RESET}")
        # 释放授权许可
        confirm_product_license(device_mac, license_data['device_id'], license_data['license_key'], 0)
        sys.exit(1)

    print(f"{GREEN}烧录授权信息成功{RESET}")


if __name__ == "__main__":
    main()
