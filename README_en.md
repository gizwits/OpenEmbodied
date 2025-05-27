# Gizwits + Coze + Xiaozhi

First, thanks to Brother Xia's open source project: https://github.com/gizwits/xiaozhi-gizwits-esp32

Second, thanks to: https://github.com/xinnan-tech/xiaozhi-esp32-server

([中文](README.md) | English | [日本語](README_ja.md))

## Production-Ready AI Solution
A commercial solution combining [Gizwits](https://www.gizwits.com/) AIOT with Coze

Supports and is compatible with all Xiaozhi-adapted hardware

You can also orchestrate more professional agents on the [Coze](https://www.coze.cn/) platform

## Network Configuration Guide

### Open the Mini Program and click the plus sign in the top right corner to configure the network
![Mini Program](docs/mini_app.png)

### Web Configuration
[Click to Jump](http://aicube.jzyjzy.club/)

## Video Introduction

👉 [ESP32+SenseVoice+Qwen72B: Create Your AI Chat Companion! 【bilibili】](https://www.bilibili.com/video/BV11msTenEH3/)

👉 [Equipping Xiaozhi with DeepSeek's Smart Brain 【bilibili】](https://www.bilibili.com/video/BV1GQP6eNEFG/)

👉 [Handcraft Your AI Girlfriend: Beginner's Tutorial 【bilibili】](https://www.bilibili.com/video/BV1XnmFYLEJN/)

## Implemented Features

- Wi-Fi / ML307 Cat.1 4G
- BOOT button wake-up and interruption, supporting both click and long-press trigger modes
- Offline voice wake-up [ESP-SR](https://github.com/espressif/esp-sr)
- Streaming voice conversation (WebSocket)
- Support for multiple languages including Mandarin, Cantonese, English, Japanese, Korean
- Large model TTS
- Large model LLM (Qwen, DeepSeek, Doubao)
- Configurable prompts and voice tones (custom character)
- Short-term memory with self-summary after each conversation
- OLED / LCD display showing signal strength or conversation content
- Support for displaying image expressions on LCD
- Multi-language support (Chinese, English)
- Camera multimodal capabilities

## ✅ Supported Chip Platforms

- ✅ ESP32-S3
- ✅ ESP32-C3
- ✅ ESP32-P4
- ✅ ESP32-C2 [ESP-SR](https://github.com/gizwits/ai-esp32-c2)

## Hardware Section

### Supported Open Source Hardware

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="LCSC ESP32-S3 Development Board">LCSC ESP32-S3 Development Board</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="Espressif ESP32-S3-BOX3">Espressif ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">AtomS3R + Echo Base</a>
- <a href="https://docs.m5stack.com/en/core/ATOM%20Matrix" target="_blank" title="AtomMatrix + Echo Base">AtomMatrix + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="Magic Button 2.4">Magic Button 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="Waveshare ESP32-S3-Touch-AMOLED-1.8">Waveshare ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="Brother Xia Mini C3">Brother Xia Mini C3</a>
- <a href="https://oshwhub.com/movecall/moji-xiaozhi-ai-derivative-editi" target="_blank" title="Movecall Moji ESP32S3">Moji Xiaozhi AI Derivative Edition</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">CuiCan AI Pendant</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="WMnologo StarZhi-1.54">WMnologo StarZhi-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>

[Image gallery section remains unchanged as it's already in HTML format]

## Firmware Section

### Development Environment

- Cursor or VSCode
- Install ESP-IDF plugin, select SDK version 5.3 or above
- Linux is better than Windows, with faster compilation speed and no driver issues
- Use Google C++ code style, ensure code meets standards when submitting

### Developer Documentation

- [Development Board Customization Guide](main/boards/README.md) - Learn how to create custom development board adaptations for Xiaozhi
- [IoT Control Module](main/iot/README.md) - Learn how to control IoT devices through AI voice

## Agent Configuration

- [Agent Publishing Guide for Gizwits Gokit5](https://ucnvydcxb9v5.feishu.cn/wiki/M51dwh0q7izeAbkm1ikcXZYtnud?from=from_copylink)
- [Create Mini Program](https://devdocs.gizwits.com/zh-cn/AppDev/Applets.html#%E5%B0%8F%E7%A8%8B%E5%BA%8F%E5%8A%9F%E8%83%BD%E7%AE%80%E4%BB%8B)
- [Agent Publishing Guide for Gizwits Gokit5](https://cb7sb1iltn.feishu.cn/docx/UikfduMgwoHWryx8vw4cRvxinSc?from=from_copylink)

## Technical Principles
- [MQTT](https://doc.weixin.qq.com/doc/w3_APAAZwbkAKUpg8ZerLGQcCFlkCuh1?scode=AFoA3gcjAA8hJRmc5YACQAaAbkAKU)
- [Coze socket](https://www.coze.cn/open/docs/developer_guides/streaming_chat_api)


## Star History

<a href="https://www.star-history.com/#gizwits/ai-esp32&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date" />
 </picture>
</a>
