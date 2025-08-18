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
            
        mac = self.mac_entry.get()
        if not mac:
            self.show_result("请输入授权MAC")
            return
        
        enterprise_id = self.enterprise_id_entry.get()
        product_key = self.product_key_entry.get()
        product_secret = self.product_secret_entry.get()
        self.report_org_license(enterprise_id, mac)
        
        response = self.allocate_org_license(enterprise_id, product_key, product_secret, mac)
        if response.status_code != 200:
            self.show_result(f"分配授权MAC（{mac}）失败：{response.text}")
            return
        
        response = self.apply_product_license(enterprise_id, product_key, product_secret, mac)
        if response.status_code != 200:
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