import requests
import json

close=0     
stanby=1    #休眠唤醒正常
running=2

data = {
    "data": [
        {
            # "id": "f525b584",
            "id": "f1290375",
            # "id": "fa42995b",
            # "id": "fe7a997f",
            
            # "id": "f2cba95f",
            "qos": 1,
            "property": {
                # "msg_flag":False,
                # "volume": 90,
                # "state":running,
                "volume_delta":-5,
                # "volume_delta":100,
            }
        }
    ]
}

json_data = json.dumps(data)
headers = {
    "Content-Type": "application/json"
}

# 使用requests库发送POST请求
response = requests.post("https://agent.gizwitsapi.com/v2/devices/status", headers=headers, data=json_data)

# 打印响应状态码和内容
print(response.status_code)
print(response.text)
