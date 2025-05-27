# Gizwits + Coze + 小智

まず、蝦哥のオープンソースプロジェクトに感謝いたします：https://github.com/gizwits/xiaozhi-gizwits-esp32

また、https://github.com/xinnan-tech/xiaozhi-esp32-server にも感謝いたします。

（[中文](README.md) | [English](README_en.md) | 日本語）

## 商用可能なAIソリューション
[Gizwits](https://www.gizwits.com/) AIOT と Coze を組み合わせた商用ソリューション

小智が対応するすべてのハードウェアをサポート

また、[Coze](https://www.coze.cn/) プラットフォームでより専門的なエージェントを構築することも可能です

## ネットワーク設定ガイド

### ミニプログラムで右上の「+」をクリックして設定
![ミニプログラム](docs/mini_app.png)

### Web設定
[クリックして移動](http://aicube.jzyjzy.club/)


## ビデオ紹介

👉 [ESP32+SenseVoice+Qwen72BでAIチャットパートナーを作ろう！【bilibili】](https://www.bilibili.com/video/BV11msTenEH3/)

👉 [小智にDeepSeekの賢い脳を搭載【bilibili】](https://www.bilibili.com/video/BV1GQP6eNEFG/)

👉 [AIガールフレンドを手作りしよう、初心者向けチュートリアル【bilibili】](https://www.bilibili.com/video/BV1XnmFYLEJN/)

## プロジェクトの目的

このプロジェクトは蝦哥によってオープンソース化され、MITライセンスで公開されており、誰でも無料で使用でき、商用利用も可能です。

このプロジェクトを通じて、より多くの人々がAIハードウェア開発の入門を支援し、急速に発展している大規模言語モデルを実際のハードウェアデバイスにどのように応用するかを理解することを願っています。AIに興味のある学生や、新しい技術を探求したい開発者など、誰でもこのプロジェクトから貴重な学習経験を得ることができます。

プロジェクトの開発と改善への参加を歓迎します。アイデアや提案がある場合は、Issueを作成するか、グループチャットに参加してください。

学習交流QQグループ：376893254

## 実装済み機能

- Wi-Fi / ML307 Cat.1 4G
- BOOTキーによる起動と中断、クリックと長押しの2つのトリガー方式をサポート
- オフライン音声起動 [ESP-SR](https://github.com/espressif/esp-sr)
- ストリーミング音声対話（WebSocketまたはUDPプロトコル）
- 中国語、広東語、英語、日本語、韓国語の5言語認識をサポート [SenseVoice](https://github.com/FunAudioLLM/SenseVoice)
- 声紋認識、AIの名前を呼んでいる人を識別 [3D Speaker](https://github.com/modelscope/3D-Speaker)
- 大規模モデルTTS（火山引擎またはCosyVoice）
- 大規模モデルLLM（Qwen、DeepSeek、Doubao）
- カスタマイズ可能なプロンプトと音声（カスタムキャラクター）
- 短期記憶、各対話後に自己要約
- OLED / LCDディスプレイ、信号強度や対話内容を表示
- LCDでの画像表情表示をサポート
- 多言語（中国語、英語）をサポート

## ✅ 対応済みチッププラットフォーム

- ✅ ESP32-S3
- ✅ ESP32-C3
- ✅ ESP32-P4

## ハードウェア部分

### ブレッドボードでの手作り実践

詳細は飛書ドキュメントチュートリアルをご覧ください：

👉 [《小智AIチャットボット百科事典》](https://ccnphfhqs21z.feishu.cn/wiki/F5krwD16viZoF0kKkvDcrZNYnhb?from=from_copylink)

ブレッドボードの効果図：

![ブレッドボード効果図](docs/wiring2.jpg)

### 対応済みオープンソースハードウェア

- <a href="https://oshwhub.com/li-chuang-kai-fa-ban/li-chuang-shi-zhan-pai-esp32-s3-kai-fa-ban" target="_blank" title="立創・実践派 ESP32-S3 開発ボード">立創・実践派 ESP32-S3 開発ボード</a>
- <a href="https://github.com/espressif/esp-box" target="_blank" title="楽鑫 ESP32-S3-BOX3">楽鑫 ESP32-S3-BOX3</a>
- <a href="https://docs.m5stack.com/zh_CN/core/CoreS3" target="_blank" title="M5Stack CoreS3">M5Stack CoreS3</a>
- <a href="https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base" target="_blank" title="AtomS3R + Echo Base">AtomS3R + Echo Base</a>
- <a href="https://docs.m5stack.com/en/core/ATOM%20Matrix" target="_blank" title="AtomMatrix + Echo Base">AtomMatrix + Echo Base</a>
- <a href="https://gf.bilibili.com/item/detail/1108782064" target="_blank" title="魔法のボタン 2.4">魔法のボタン 2.4</a>
- <a href="https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.8.htm" target="_blank" title="微雪電子 ESP32-S3-Touch-AMOLED-1.8">微雪電子 ESP32-S3-Touch-AMOLED-1.8</a>
- <a href="https://github.com/Xinyuan-LilyGO/T-Circle-S3" target="_blank" title="LILYGO T-Circle-S3">LILYGO T-Circle-S3</a>
- <a href="https://oshwhub.com/tenclass01/xmini_c3" target="_blank" title="蝦哥 Mini C3">蝦哥 Mini C3</a>
- <a href="https://oshwhub.com/movecall/moji-xiaozhi-ai-derivative-editi" target="_blank" title="Movecall Moji ESP32S3">Moji 小智AI派生版</a>
- <a href="https://oshwhub.com/movecall/cuican-ai-pendant-lights-up-y" target="_blank" title="Movecall CuiCan ESP32S3">璀璨・AIペンダント</a>
- <a href="https://github.com/WMnologo/xingzhi-ai" target="_blank" title="無名科技Nologo-星智-1.54">無名科技Nologo-星智-1.54TFT</a>
- <a href="https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html" target="_blank" title="SenseCAP Watcher">SenseCAP Watcher</a>
<div style="display: flex; justify-content: space-between;">
  <a href="docs/v1/lichuang-s3.jpg" target="_blank" title="立創・実践派 ESP32-S3 開発ボード">
    <img src="docs/v1/lichuang-s3.jpg" width="240" />
  </a>
  <a href="docs/v1/espbox3.jpg" target="_blank" title="楽鑫 ESP32-S3-BOX3">
    <img src="docs/v1/espbox3.jpg" width="240" />
  </a>
  <a href="docs/v1/m5cores3.jpg" target="_blank" title="M5Stack CoreS3">
    <img src="docs/v1/m5cores3.jpg" width="240" />
  </a>
  <a href="docs/v1/atoms3r.jpg" target="_blank" title="AtomS3R + Echo Base">
    <img src="docs/v1/atoms3r.jpg" width="240" />
  </a>
  <a href="docs/v1/magiclick.jpg" target="_blank" title="魔法のボタン 2.4">
    <img src="docs/v1/magiclick.jpg" width="240" />
  </a>
  <a href="docs/v1/waveshare.jpg" target="_blank" title="微雪電子 ESP32-S3-Touch-AMOLED-1.8">
    <img src="docs/v1/waveshare.jpg" width="240" />
  </a>
  <a href="docs/lilygo-t-circle-s3.jpg" target="_blank" title="LILYGO T-Circle-S3">
    <img src="docs/lilygo-t-circle-s3.jpg" width="240" />
  </a>
  <a href="docs/xmini-c3.jpg" target="_blank" title="蝦哥 Mini C3">
    <img src="docs/xmini-c3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-moji-esp32s3.jpg" target="_blank" title="Movecall Moji 小智AI派生版">
    <img src="docs/v1/movecall-moji-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/movecall-cuican-esp32s3.jpg" target="_blank" title="CuiCan">
    <img src="docs/v1/movecall-cuican-esp32s3.jpg" width="240" />
  </a>
  <a href="docs/v1/wmnologo_xingzhi_1.54.jpg" target="_blank" title="無名科技Nologo-星智-1.54">
    <img src="docs/v1/wmnologo_xingzhi_1.54.jpg" width="240" />
  </a>
  <a href="docs/v1/sensecap_watcher.jpg" target="_blank" title="SenseCAP Watcher">
    <img src="docs/v1/sensecap_watcher.jpg" width="240" />
  </a>
</div>

## ファームウェア部分

### 開発環境不要の書き込み

初心者の方は、まず開発環境を構築せずに、開発環境不要の書き込み用ファームウェアを使用することをお勧めします。

ファームウェアはデフォルトで [xiaozhi.me](https://xiaozhi.me) 公式サーバーに接続し、現在個人ユーザーはアカウント登録でQwenリアルタイムモデルを無料で使用できます。

👉 [Flash書き込みファームウェア（IDF開発環境不要）](https://ccnphfhqs21z.feishu.cn/wiki/Zpz4wXBtdimBrLk25WdcXzxcnNS) 


### 開発環境

- Cursor または VSCode
- ESP-IDFプラグインをインストールし、SDKバージョン5.3以上を選択
- LinuxはWindowsよりも優れており、コンパイル速度が速く、ドライバーの問題も回避できます
- Google C++コードスタイルを使用し、コード提出時は規約に準拠していることを確認してください

### 開発者ドキュメント

- [開発ボードカスタマイズガイド](main/boards/README.md) - 小智用のカスタム開発ボードアダプターの作成方法を学ぶ
- [IoT制御モジュール](main/iot/README.md) - AI音声によるIoTデバイス制御の方法を理解する


## エージェント設定

小智AIチャットボットデバイスをお持ちの方は、[xiaozhi.me](https://xiaozhi.me) コンソールで設定できます。

👉 [バックエンド操作ビデオチュートリアル（旧インターフェース）](https://www.bilibili.com/video/BV1jUCUY2EKM/)

## 技術原理とプライベートデプロイ

👉 [詳細なWebSocket通信プロトコルドキュメント](docs/websocket.md)

個人PCでサーバーをデプロイする場合は、MITライセンスでオープンソース化された別の作者のプロジェクト [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) を参照してください。

## Star History

<a href="https://www.star-history.com/#gizwits/ai-esp32&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=gizwits/ai-esp32&type=Date" />
 </picture>
</a>
