# Gizwits + Coze + 小智

首先致谢虾哥的开源项目：https://github.com/gizwits/xiaozhi-gizwits-esp32

其次致谢：https://github.com/xinnan-tech/xiaozhi-esp32-server

（中文 | [English](README_en.md) | [日本語](README_ja.md)）

## 可以量产的AI方案
[Gizwits](https://www.gizwits.com/) AIOT 结合 Coze 打造的可商用方案

支持和兼容所有小智适配的硬件

同时您也可以在 [Coze](https://www.coze.cn/) 平台上编排更专业的智能体

## 配网指引

### 打开小程序点击右上角加号配网
![小程序](docs/mini_app.png)

### web配网
[点击跳转](http://aicube.jzyjzy.club/)


## 视频介绍

👉 [ESP32+SenseVoice+Qwen72B打造你的AI聊天伴侣！【bilibili】](https://www.bilibili.com/video/BV11msTenEH3/)

👉 [给小智装上 DeepSeek 的聪明大脑【bilibili】](https://www.bilibili.com/video/BV1GQP6eNEFG/)

👉 [手工打造你的 AI 女友，新手入门教程【bilibili】](https://www.bilibili.com/video/BV1XnmFYLEJN/)

## 项目目的

本项目是由虾哥开源的一个开源项目，以 MIT 许可证发布，允许任何人免费使用，并可以用于商业用途。

我们希望通过这个项目，能够帮助更多人入门 AI 硬件开发，了解如何将当下飞速发展的大语言模型应用到实际的硬件设备中。无论你是对 AI 感兴趣的学生，还是想要探索新技术的开发者，都可以通过这个项目获得宝贵的学习经验。

欢迎所有人参与到项目的开发和改进中来。如果你有任何想法或建议，请随时提出 Issue 或加入群聊。

学习交流 QQ 群：376893254

## 已实现功能

- Wi-Fi / ML307 Cat.1 4G
- BOOT 键唤醒和打断，支持点击和长按两种触发方式
- 离线语音唤醒 [ESP-SR](https://github.com/espressif/esp-sr)
- 流式语音对话（WebSocket）
- 支持国语、粤语、英语、日语、韩语 等多种语言识别
- 大模型 TTS
- 大模型 LLM（Qwen, DeepSeek, Doubao）
- 可配置的提示词和音色（自定义角色）
- 短期记忆，每轮对话后自我总结
- OLED / LCD 显示屏，显示信号强弱或对话内容
- 支持 LCD 显示图片表情
- 支持多语言（中文、英文）

## ✅ 已支持的芯片平台

- ✅ ESP32-S3
- ✅ ESP32-C3
- ✅ ESP32-P4
- ✅ ESP32-C2 [ESP-SR]([https://github.com/espressif/esp-sr](https://github.com/gizwits/ai-esp32-c2))

## 硬件部分

### 已支持的开源硬件

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="立创·实战派 ESP32-S3 开发板">立创·实战派 ESP32-S3 开发板</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="乐鑫 ESP32-S3-BOX3">乐鑫 ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">AtomS3R + Echo Base</a>
- <a href="https://docs.m5stack.com/en/core/ATOM%20Matrix" target="_blank" title="AtomMatrix + Echo Base">AtomMatrix + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="神奇按钮 2.4">神奇按钮 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="微雪电子 ESP32-S3-Touch-AMOLED-1.8">微雪电子 ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="虾哥 Mini C3">虾哥 Mini C3</a>
- <a href="https://oshwhub.com/movecall/moji-xiaozhi-ai-derivative-editi" target="_blank" title="Movecall Moji ESP32S3">Moji 小智AI衍生版</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">璀璨·AI吊坠</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="无名科技Nologo-星智-1.54">无名科技Nologo-星智-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>
<div style="display: flex; justify-content: space-between;">
  <a href="docs/v1/lichuang-s3.jpg" target="_blank" title="立创·实战派 ESP32-S3 开发板">
    <img src="docs/v1/lichuang-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/espbox3.jpg" target="_blank" title="乐鑫 ESP32-S3-BOX3">
    <img src="docs/v1/espbox3.jpg" width="240" />
  </a>
  <a href="docs/v1/m5cores3.jpg" target="_blank" title="M5Stack CoreS3">
    <img src="docs/v1/m5cores3.jpg" width="240" />
  </a>
  <a href="docs/v1/atoms3r.jpg" target="_blank" title="AtomS3R + Echo Base">
    <img src="docs/v1/atoms3r.jpg" width="240" />
  </a>
  <a href="docs/v1/magiclick.jpg" target="_blank" title="神奇按钮 2.4">
    <img src="docs/v1/magiclick.jpg" width="240" />
  </a>
  <a href="docs/v1/waveshare.jpg" target="_blank" title="微雪电子 ESP32-S3-Touch-AMOLED-1.8">
    <img src="docs/v1/waveshare.jpg" width="240" />
  </a>
  <a href="docs/lilygo-t-circle-s3.jpg" target="_blank" title="LILYGO T-Circle-S3">
    <img src="docs/lilygo-t-circle-s3.jpg" width="240" />
  </a>
  <a href="docs/xmini-c3.jpg" target="_blank" title="虾哥 Mini C3">
    <img src="docs/xmini-c3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-moji-esp32s3.jpg" target="_blank" title="Movecall Moji 小智AI衍生版">
    <img src="docs/v1/movecall-moji-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-cuican-esp32s3.jpg" target="_blank" title="CuiCan">
    <img src="docs/v1/movecall-cuican-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/wmnologo_xingzhi_1.54.jpg" target="_blank" title="无名科技Nologo-星智-1.54">
    <img src="docs/v1/wmnologo_xingzhi_1.54.jpg" width="240" />
  </a>
  <a href="docs/v1/sensecap_watcher.jpg" target="_blank" title="SenseCAP Watcher">
    <img src="docs/v1/sensecap_watcher.jpg" width="240" />
  </a>
</div>

## 固件部分

### 开发环境

- Cursor 或 VSCode
- 安装 ESP-IDF 插件，选择 SDK 版本 5.3 或以上
- Linux 比 Windows 更好，编译速度快，也免去驱动问题的困扰
- 使用 Google C++ 代码风格，提交代码时请确保符合规范

### 开发者文档

- [开发板定制指南](main/boards/README.md) - 学习如何为小智创建自定义开发板适配
- [物联网控制模块](main/iot/README.md) - 了解如何通过AI语音控制物联网设备


## 智能体配置

- [智能体发布到机智云Gokit5说明](https://ucnvydcxb9v5.feishu.cn/wiki/M51dwh0q7izeAbkm1ikcXZYtnud?from=from_copylink)
- [创建小程序](https://devdocs.gizwits.com/zh-cn/AppDev/Applets.html#%E5%B0%8F%E7%A8%8B%E5%BA%8F%E5%8A%9F%E8%83%BD%E7%AE%80%E4%BB%8B)
- [智能体发布到机智云Gokit5说明](https://cb7sb1iltn.feishu.cn/docx/UikfduMgwoHWryx8vw4cRvxinSc?from=from_copylink)

## 技术原理
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
