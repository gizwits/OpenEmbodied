#!/usr/bin/env python3

import os
import sys
import subprocess
import json
import stat
import requests
import json
import time

PRODUCT_KEY = "a54283350726462daaeab498ffee87de"  # 产品密钥
PRODUCT_SECRET = "697d826fcc2642fbae7949683e3cf8e0"  # 产品密钥
ORG_ID = 16271

def report_org_auth(mac):
    url = f"http://43.139.79.34:31647/v4/organizations/{ORG_ID}/licenses"
    
    # 使用 form-data 格式提交
    data = {
        'device_mac': mac
    }
    
    try:
        response = requests.post(url, data=data)
        response.raise_for_status()  # 检查响应状态
        print(response.text)
        return response.json()
    except requests.exceptions.RequestException as e:
        print(f"Error reporting MAC: {e}", file=sys.stderr)
        return None

def apply_auth(mac):
    
    url = f"http://43.139.79.34:31647/v4/products/{PRODUCT_KEY}/licenses"

    payload = json.dumps({
    "product_secret": PRODUCT_SECRET,
    "count": 1,
    "device_macs": [
        mac
    ]
    })
    headers = {
        'X-Org-Id': f'{ORG_ID}',
        'Content-Type': 'application/json'
    }

    response = requests.request("POST", url, headers=headers, data=payload)

    print(response.text)
    return response.json()

def allocation_auth(mac):

    url = f"http://43.139.79.34:31647/v4/organizations/{ORG_ID}/licenses"

    payload = json.dumps({
    "product_key": PRODUCT_KEY,
    "product_secret": PRODUCT_SECRET,
    "count": 1,
    "device_macs": [
        mac
    ]
    })
    headers = {
    'Content-Type': 'application/json'
    }

    response = requests.request("PUT", url, headers=headers, data=payload)

    # 转换成json
    return response.json()

def confirm_auth(device_mac, device_id, license_key):
    
    url = f"http://43.139.79.34:31647/v4/products/{PRODUCT_KEY}/licenses/{license_key}"
    print(url)
    payload = json.dumps({
    "product_secret": PRODUCT_SECRET,
    "device_id": f"{device_id}",
    "device_mac": f"{device_mac}",
    "state": 3
    })
    headers = {
    'X-Org-Id': f'{ORG_ID}',
    'Content-Type': 'application/json'
    }

    print(payload)
    response = requests.request("PUT", url, headers=headers, data=payload)

    print(response.text)
    return response.json()


# 脚本所在目录
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def get_mac():
    """获取设备 MAC 地址"""
    try:
        # 使用esptool.exe获取MAC地址
        cmd = ['esptool.exe', 'read_mac']
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
                    print(f"Got MAC address: {mac}")
                    return mac
            print("MAC address not found in output", file=sys.stderr)
            return None
        else:
            print(f"Error getting MAC: {result.stderr}", file=sys.stderr)
            return None
    except Exception as e:
        print(f"Exception while getting MAC: {e}", file=sys.stderr)
        return None

def write_auth_to_flash(auth_key, device_id):
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
        # 写入到临时文件
        with open("auth_data.bin", "w") as f:
            f.write(full_data)
        
        # 使用esptool写入flash
        cmd = ['./esptool.exe', 'write_flash', '0x744000', 'auth_data.bin']
        result = subprocess.run(cmd, capture_output=True, text=True)
        print(result)
        if result.returncode == 0:
            print(f"Written data: {full_data}")
            return True
        else:
            print(f"Error writing to flash: {result.stderr}", file=sys.stderr)
            return False
            
    except Exception as e:
        print(f"Exception while writing auth: {e}", file=sys.stderr)
        return False

def main():
    # 获取 MAC 地址
    mac = get_mac()
    if not mac:
        sys.exit(1)
    
    print(mac)
    report_org_auth(mac)
    time.sleep(4)
    allocation_auth(mac)
   
    json_res = apply_auth(mac)
    print(json_res)
    json_data = json_res['data'][0]
    print(json_data)

    print("授权成功")
    # 直接调用写入flash的函数
    if write_auth_to_flash(json_data["license_key"], json_data["device_id"]):
        print("写入成功")
        confirm_res = confirm_auth(mac, json_data['device_id'], json_data['license_key'])
        print(confirm_res)
    else:
        print("写入失败")
        sys.exit(1)


if __name__ == "__main__":
    main()
