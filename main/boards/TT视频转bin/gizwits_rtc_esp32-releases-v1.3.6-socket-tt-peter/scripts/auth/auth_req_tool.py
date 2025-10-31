import tkinter as tk
from tkinter import ttk, scrolledtext
import requests
import json

class AuthTool:
    def __init__(self, root):
        self.root = root
        self.root.title("授权工具")
        self.root.geometry("500x500")  # 调整高度为500像素
        
        # 设置主题样式
        style = ttk.Style()
        style.configure('TFrame', background='#f0f0f0')
        style.configure('TLabelframe', background='#f0f0f0', relief='solid', borderwidth=1)
        style.configure('TLabelframe.Label', font=('Arial', 14, 'bold'), background='#f0f0f0')
        style.configure('TLabel', background='#f0f0f0', font=('Arial', 13))
        style.configure('TButton', font=('Arial', 13))
        style.configure('TEntry', font=('Arial', 13))
        
        # 创建主框架
        self.main_frame = ttk.Frame(root, padding="10")
        self.main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # 配置网格权重
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        self.main_frame.columnconfigure(0, weight=1)
        self.main_frame.rowconfigure(2, weight=1)  # 结果框所在行
        
        # 企业信息和产品信息部分
        self.create_enterprise_product_section()
        
        # 授权MAC部分
        self.create_auth_section()
        
        # 结果显示区域
        self.create_result_section()
        
        # 状态变量
        self.enterprise_verified = False
        self.product_verified = False
        self.mac_verified = False
        
    def create_enterprise_product_section(self):
        ep_frame = ttk.LabelFrame(self.main_frame, text="企业及产品信息", padding="5")
        ep_frame.grid(row=0, column=0, sticky=(tk.W, tk.E), pady=5)
        ep_frame.columnconfigure(1, weight=1)
        
        # 企业ID
        ttk.Label(ep_frame, text="企业ID:").grid(row=0, column=0, padx=2, pady=2, sticky=tk.W)
        self.enterprise_id_entry = ttk.Entry(ep_frame, width=35)
        self.enterprise_id_entry.grid(row=0, column=1, padx=2, pady=2, sticky=(tk.W, tk.E))
        
        # 产品Key
        ttk.Label(ep_frame, text="产品Key:").grid(row=1, column=0, padx=2, pady=2, sticky=tk.W)
        self.product_key_entry = ttk.Entry(ep_frame, width=32)
        self.product_key_entry.grid(row=1, column=1, padx=2, pady=2, sticky=(tk.W, tk.E))
        # 添加输入验证
        self.product_key_entry.bind('<KeyRelease>', self.validate_product_key)
        
        # 产品Secret
        ttk.Label(ep_frame, text="产品Secret:").grid(row=2, column=0, padx=2, pady=2, sticky=tk.W)
        self.product_secret_entry = ttk.Entry(ep_frame, width=35)
        self.product_secret_entry.grid(row=2, column=1, padx=2, pady=2, sticky=(tk.W, tk.E))
        
        # 确认按钮 - 横跨三行
        self.ep_verify_button = ttk.Button(ep_frame, text="确认", command=self.verify_enterprise_product)
        self.ep_verify_button.grid(row=0, column=2, rowspan=3, padx=3, pady=2, sticky=(tk.N, tk.S))
        
    def validate_product_key(self, event):
        key = self.product_key_entry.get()
        if len(key) > 32:
            self.product_key_entry.delete(32, tk.END)
            self.show_result("产品Key长度不能超过32位")
            
    def create_auth_section(self):
        auth_frame = ttk.LabelFrame(self.main_frame, text="授权信息", padding="5")
        auth_frame.grid(row=1, column=0, sticky=(tk.W, tk.E), pady=5)
        auth_frame.columnconfigure(1, weight=1)
        
        ttk.Label(auth_frame, text="授权MAC:").grid(row=0, column=0, padx=2, pady=2, sticky=tk.W)
        self.mac_entry = ttk.Entry(auth_frame, width=35)
        self.mac_entry.grid(row=0, column=1, padx=2, pady=2, sticky=(tk.W, tk.E))
        
        self.auth_button = ttk.Button(auth_frame, text="授权", command=self.authorize_device)
        self.auth_button.grid(row=0, column=2, padx=3, pady=2)
        
    def create_result_section(self):
        result_frame = ttk.LabelFrame(self.main_frame, text="结果", padding="5")
        result_frame.grid(row=2, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        result_frame.columnconfigure(0, weight=1)
        result_frame.rowconfigure(0, weight=1)
        
        # 创建带滚动条的文本框
        self.result_text = scrolledtext.ScrolledText(
            result_frame,
            wrap=tk.WORD,
            width=35,
            height=10,
            font=('Arial', 13)
        )
        self.result_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), padx=2, pady=2)
        
    def verify_enterprise_product(self):
        enterprise_id = self.enterprise_id_entry.get()
        product_key = self.product_key_entry.get()
        product_secret = self.product_secret_entry.get()
        
        if not enterprise_id or not product_key or not product_secret:
            self.show_result("请填写完整的企业和产品信息")
            return
            
        if len(product_key) > 32:
            self.show_result("产品Key长度不能超过32位")
            return
            
        # 获取企业授权数
        try:
            url = f"http://43.139.79.34:31647/v4/organizations/{enterprise_id}/licenses"
            response = requests.get(url)
            response.raise_for_status()  # 检查响应状态码
            
            data = response.json()
            self.enterprise_verified = True

            for product_info in data['data']['products']:
                if product_info['product_key'] == product_key:
                    self.product_verified = True
                    break

            if not self.product_verified:
                self.show_result(f"产品Key无效，请检查产品Key是否正确")
                raise
        except requests.exceptions.RequestException as e:
            self.show_result(f"获取企业授权信息失败")
            return
        
        self.show_result(f"获取企业及产品授权信息成功")
        
        self.lock_enterprise_product_fields()

    def report_org_license(self, enterprise_id, mac):
        url = f"http://43.139.79.34:31647/v4/organizations/{enterprise_id}/licenses"
        data = {
            'device_mac': mac
        }
        response = requests.post(url, data=data, timeout=5)
        return response


    def allocate_org_license(self, enterprise_id, product_key, product_secret, mac):
        url = f"http://43.139.79.34:31647/v4/organizations/{enterprise_id}/licenses"
        headers = {
            'Content-Type': 'application/json'
        }
        payload = json.dumps({
            "product_key": product_key,
            "product_secret": product_secret,
            "device_macs": [
                mac
            ]
        })
        response = requests.request("PUT", url, headers=headers, data=payload, timeout=5)
        return response


    def apply_product_license(self, enterprise_id, product_key, product_secret, mac):
        url = f"http://43.139.79.34:31647/v4/products/{product_key}/licenses"
        headers = {
            'X-Org-Id': f'{enterprise_id}',
            'Content-Type': 'application/json'
        }
        payload = json.dumps({
            "product_secret": product_secret,
            "device_macs": [
                mac
            ]
        })
        response = requests.request("POST", url, headers=headers, data=payload, timeout=5)
        return response


    def confirm_product_license(self, enterprise_id, product_key, product_secret, device_mac, device_id, license_key):
        url = f"http://43.139.79.34:31647/v4/products/{product_key}/licenses/{license_key}"
        payload = json.dumps({
            "product_secret": product_secret,
            "device_id": f"{device_id}",
            "device_mac": f"{device_mac}",
            "state": 3
        })
        headers = {
            'X-Org-Id': f'{enterprise_id}',
            'Content-Type': 'application/json'
        }
        response = requests.request("PUT", url, headers=headers, data=payload, timeout=5)
        return response


    def authorize_device(self):
        if not self.enterprise_verified or not self.product_verified:
            self.show_result("请先验证企业和产品信息")
            return
            
        mac = self.mac_entry.get().lower()
        if not mac:
            self.show_result("请输入授权MAC")
            return
        
        enterprise_id = self.enterprise_id_entry.get()
        product_key = self.product_key_entry.get()
        product_secret = self.product_secret_entry.get()
        self.report_org_license(enterprise_id, mac)
        
        response = self.allocate_org_license(enterprise_id, product_key, product_secret, mac)
        print("allocate_org_license:", response.text)
        if response.status_code != 200:
            self.show_result(f"分配授权MAC（{mac}）失败：{response.text}")
            return
        
        response = self.apply_product_license(enterprise_id, product_key, product_secret, mac)
        print("allocate_org_license:", response.text)
        if response.status_code != 200 or len(response.json().get('data')) == 0:
            self.show_result(f"申请授权MAC（{mac}）失败：{response.text}")
            return
        
        license_data = response.json().get('data')[0]
        response = self.confirm_product_license(enterprise_id, product_key, product_secret, mac, license_data['device_id'], license_data['license_key'])
        if response.status_code != 200:
            self.show_result(f"确认授权MAC（{mac}）失败：{response.text}")
            return
        
        self.mac_verified = True
        self.show_result(f"授权成功\n产品Key: {product_key}\n设备MAC: {mac}\n设备ID: {license_data['device_id']}\n设备授权码: {license_data['license_key']}")
        
    def lock_enterprise_product_fields(self):
        self.enterprise_id_entry.config(state='disabled')
        self.product_key_entry.config(state='disabled')
        self.product_secret_entry.config(state='disabled')
        self.ep_verify_button.config(state='disabled')
        
    def show_result(self, message):
        self.result_text.delete(1.0, tk.END)
        self.result_text.insert(tk.END, message)


    
if __name__ == "__main__":
    root = tk.Tk()
    app = AuthTool(root)
    root.mainloop() 
    
    
    
    
# TT music 用这个
# 企业ID 16443
#define PRODUCT_KEY     "8179cb7ac34649fe9eaa735892aed562"  // 产品密钥
#define PRODUCT_SECRET  "57125a1eee4c484ebbd806406201dc5a"  // 产品密钥

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf284
# 设备ID: fa42995b
# 设备授权码: 30aaff787d8242fb94f9cd3b54d548f3

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf318
# 设备ID: f2cba95f
# 设备授权码: 5a9aed660fb34d84a024dd82ff33059a



# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf154
# 设备ID: fa79e30d
# 设备授权码: 375d3c2c38e745498411d0534c6a6a52

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf294
# 设备ID: f7233638
# 设备授权码: 17fe77416e0e408ba592e5dac40eec2b

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf1cc
# 设备ID: ff339b59
# 设备授权码: 7a69e844d09d464997ccbb0b14d06177

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf2a8
# 设备ID: fd96c845
# 设备授权码: afe100e44be343f4a7ff5d0f994068e2

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf19c
# 设备ID: f3de9fe4
# 设备授权码: ff063fd12089447286c9e9aed03641ee

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf0d4
# 设备ID: f7cf1443
# 设备授权码: 3c66cbcc8a1146698c64d885affd2d30

# 授权成功
# 产品Key: 8179cb7ac34649fe9eaa735892aed562
# 设备MAC: cc8da20bf26c
# 设备ID: fa1cc2c0
# 设备授权码: 12d31156cdf24486a6120db2ba225a28

#if 1   // 4G-广和通（组织ID：16271）
#define PRODUCT_KEY     "a3222d21e06d48068582508e2b61519e"  // 产品密钥
#define PRODUCT_SECRET  "0ba88a310290432b866881ac572e7249"  // 产品密钥
#define TEST_USER       "6499e3651fd94dc6926a4103391e9b7e"  // MAC地址


#MAC直接写 字符串

# 授权成功
# 产品Key:8179cb7ac34649fe9eaa735892aed562
# 设备MAC: e80690a84e6c
# 设备ID:f1952f11
# 设备授权码: 4af0435dff3f42b5ad773a664e4bcaf8