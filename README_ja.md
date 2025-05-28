# Gizwits + Coze + 小智

まず、蝦哥のオープンソースプロジェクトに感謝いたします：https://github.com/78/xiaozhi-esp32

次に、https://github.com/xinnan-tech/xiaozhi-esp32-server にも感謝いたします。

（[中文](README.md) | [English](README_en.md) | 日本語）

## 量産可能なAIソリューション
[Gizwits](https://www.gizwits.com/) AIOTとCozeを組み合わせた商用ソリューション

小智が対応するすべてのハードウェアをサポート

また、[Coze](https://www.coze.cn/)プラットフォームでより専門的なエージェントを構築することも可能です

## ネットワーク設定ガイド

### ミニプログラムで右上の「+」をクリックして設定
![ミニプログラム](docs/mini_app.png)

### Web設定
[クリックして移動](http://aicube.jzyjzy.club/)


## ビデオ紹介

👉 [ESP32+SenseVoice+Qwen72BでAIチャットパートナーを作ろう！【bilibili】](https://www.bilibili.com/video/BV11msTenEH3/)

👉 [小智にDeepSeekの賢い脳を搭載【bilibili】](https://www.bilibili.com/video/BV1GQP6eNEFG/)

👉 [手作りAIガールフレンド、初心者向けチュートリアル【bilibili】](https://www.bilibili.com/video/BV1XnmFYLEJN/)

## 実装済み機能

- Wi-Fi / ML307 Cat.1 4G
- BOOTキーによる起動と中断、クリックと長押しの2つのトリガー方式をサポート
- オフライン音声起動 [ESP-SR](https://github.com/espressif/esp-sr)
- ストリーミング音声対話（WebSocket）
- 中国語、広東語、英語、日本語、韓国語など複数言語の認識をサポート
- 大規模言語モデルTTS
- 大規模言語モデルLLM（Qwen、DeepSeek、Doubao）
- カスタマイズ可能なプロンプトと音声（カスタムキャラクター）
- 短期記憶、各対話後の自己要約
- OLED / LCDディスプレイ、信号強度や対話内容の表示
- LCDでの画像表情表示をサポート
- 多言語対応（中国語、英語）
- カメラマルチモーダル

## ✅ 対応済みチッププラットフォーム

- ✅ ESP32-S3
- ✅ ESP32-C3
- ✅ ESP32-P4
- ✅ ESP32-C2 [ESP-SR](https://github.com/gizwits/ai-esp32-c2)

## ハードウェア部分

### 対応済みオープンソースハードウェア

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="立創・実践派 ESP32-S3 開発ボード">立創・実践派 ESP32-S3 開発ボード</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="Espressif ESP32-S3-BOX3">Espressif ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">AtomS3R + Echo Base</a>
- <a href="https://docs.m5stack.com/en/core/ATOM%20Matrix" target="_blank" title="AtomMatrix + Echo Base">AtomMatrix + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="マジックボタン 2.4">マジックボタン 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="Waveshare ESP32-S3-Touch-AMOLED-1.8">Waveshare ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="蝦哥 Mini C3">蝦哥 Mini C3</a>
- <a href="https://oshwhub.com/movecall/moji-xiaozhi-ai-derivative-editi" target="_blank" title="Movecall Moji ESP32S3">Moji 小智AI派生版</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">璀璨・AIペンダント</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="無名科技Nologo-星智-1.54">無名科技Nologo-星智-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>

## ファームウェア部分

### 開発環境

- Cursor または VSCode
- ESP-IDFプラグインをインストール、SDKバージョン5.3以上を選択
- WindowsよりLinuxの方が推奨（コンパイル速度が速く、ドライバーの問題も回避可能）
- Google C++コードスタイルを使用、コード提出時は規約に準拠していることを確認

### 開発者ドキュメント

- [開発ボードカスタマイズガイド](main/boards/README.md) - 小智用のカスタム開発ボードアダプターの作成方法を学ぶ
- [IoT制御モジュール](main/iot/README.md) - AI音声によるIoTデバイス制御の方法を理解する

## エージェント設定

- [エージェントのGizwits Gokit5への公開手順](https://ucnvydcxb9v5.feishu.cn/wiki/M51dwh0q7izeAbkm1ikcXZYtnud?from=from_copylink)
- [ミニプログラムの作成](https://devdocs.gizwits.com/zh-cn/AppDev/Applets.html#%E5%B0%8F%E7%A8%8B%E5%BA%8F%E5%8A%9F%E8%83%BD%E7%AE%80%E4%BB%8B)
- [エージェントのGizwits Gokit5への公開手順](https://cb7sb1iltn.feishu.cn/docx/UikfduMgwoHWryx8vw4cRvxinSc?from=from_copylink)

## 技術原理
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
