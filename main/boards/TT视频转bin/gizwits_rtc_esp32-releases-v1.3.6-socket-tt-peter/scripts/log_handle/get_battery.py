import re
import matplotlib.pyplot as plt
import pandas as pd
from datetime import datetime, timedelta

def parse_battery_data(file_path):
    time_list = []
    voltage_list = []

    with open(file_path, 'rb') as file:  # 修改为使用二进制模式打开文件
        for line in file:
            line = line.decode('utf-8', errors='ignore')  # 忽略解码错误
            # 使用正则表达式提取时间和电压
            match = re.search(r'\[(\d{2}:\d{2}:\d{2})\.\d{3}\].*?Battery ADC voltage:(\d+)', line)
            if match:
                time = match.group(1)
                voltage = int(match.group(2))
                time_list.append(time)
                voltage_list.append(voltage)

    return time_list, voltage_list

def create_voltage_table(time_list, voltage_list):
    # 创建DataFrame，包含时间和电压
    df = pd.DataFrame({'时间': time_list, '电压': voltage_list})
    
    # 将DataFrame保存为CSV文件
    df.to_csv('voltage_table.csv', encoding='utf-8-sig', index=False)
    
    return df

def plot_battery_data(time_list, voltage_list):
    # 创建电压表格
    voltage_table = create_voltage_table(time_list, voltage_list)
    
    plt.figure(figsize=(10, 5))
    plt.plot(time_list, voltage_list, marker='o')
    plt.xlabel('时间')  # 修改为中文标签
    plt.ylabel('电池电压 (mV)')  # 修改为中文标签
    plt.title('电池电压随时间变化')  # 修改为中文标题
    plt.xticks(rotation=45)
    
    # 设置刻度
    plt.gca().xaxis.set_major_locator(plt.MultipleLocator(30))  # 30秒的刻度
    plt.gca().yaxis.set_major_locator(plt.MultipleLocator(50))  # 50mV的刻度

    plt.tight_layout()
    plt.show()

# 示例用法
file_path = '最低3v的电池续航日志.log'  # 替换为实际的文件路径
time_list, voltage_list = parse_battery_data(file_path)
plot_battery_data(time_list, voltage_list)
