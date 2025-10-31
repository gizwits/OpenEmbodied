# u = 'http://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/push_short_16k.mp3~tplv-gztkv53tgq-image.image?lk3s=fff2e91a&x-expires=1768232892&x-signature=4DU2GRA%2BbO1BI8eRF5ZXlaMxy30%3D'
# u = 'https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/ac386a6f804443d48067f71fe7574560.mp3~tplv-gztkv53tgq-image.image?lk3s=76f83bd8&x-expires=1778231492&x-signature=oIaTFmpagULtbJxxWpjyosw7EUw%3D'
# u = 'https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/ccb0bd2c40e449fa9b7ae8c3ea89d037.mp3~tplv-gztkv53tgq-image.image?lk3s=fff2e91a&x-expires=1747379609&x-signature=IHtx7aAokd7JSbWmxOWS0CaKuhs%3D'
# u = 'https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/ac386a6f804443d48067f71fe7574560.mp3~tplv-gztkv53tgq-image.image?lk3s=76f83bd8&x-expires=1778231492&x-signature=oIaTFmpagULtbJxxWpjyosw7EUw%3D'
# 长音频测试
# u = 'https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/first_voice_letter_24k.mp3~tplv-gztkv53tgq-image.image?lk3s=76f83bd8&x-expires=1755676430&x-signature=ZrYdgDXiwlz8bY0RVdrLAAFWMDs%3D'
u ='https://p11-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/first_voice_1_16k.mp3~tplv-gztkv53tgq-image.image?lk3s=76f83bd8&x-expires=1757654427&x-signature=WsvYPaTP77m0m3hm4oPW1jBdCTE%3D'
u.encode().ljust(256, b'\x00').hex()
print(u.encode().ljust(256, b'\x00').hex())

import requests
import json

import random

def generate_random_msg_id(length=19):
    random_id = ''.join(random.choice('0123456789') for _ in range(length))
    padded_id = random_id.ljust(19, '0')
    ascii_id = ''.join(str(ord(char)) for char in padded_id)
    return ascii_id

hex_url = u.encode().ljust(256, b'\x00').hex()
data = {
    "data": [
        {
            # "id": "f45b1454",
            "id": "f1c8faf2",
            # "id": "fe7a997f",
            # "id": "f525b584",
            # "id": "fe7a997f",
            "qos": 1,
            "property": {
                "msg_flag": True,
                "msg_id": generate_random_msg_id(),
                "msg_type": 2,
                "msg_url": hex_url
            }
        }
    ]
}
print(data["data"][0]["property"]["msg_id"])

json_data = json.dumps(data)
headers = {
    "Content-Type": "application/json"
}

# 使用requests库发送POST请求
response = requests.post("https://agent.gizwitsapi.com/v2/devices/status", headers=headers, data=json_data)

# 打印响应状态码和内容
print(response.status_code)
print(response.text)
