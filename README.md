# DVC-PICO v2 Controller & Analyzer

RP2040（Raspberry Pi Pico / Pico 2）をベースにした、多機能なDMX-512コントローラーおよびオシロスコープ・アナライザープロジェクトです。
ハードウェア（KiCad設計データ）とファームウェア（Arduinoスケッチ）が一体となって管理されています。

https://qiita.com/kurumi18891/private/b40f511b77fc0b5f25af

https://qiita.com/kurumi18891/private/ef590465823563e47984

---

## 📂 フォルダ構成

```
DVC_pico2_1/ (リポジトリルート)
├── DVC_pico2_1.ino        # メインファームウェア (Arduinoスケッチ)
├── .gitignore             # Git除外設定ファイル
├── README.md              # 本ドキュメント
├── antigra_DVCpico/       # 補助フォルダ
└── kicad_DVC_pico/        # ハードウェア設計データ
    └── DVC-picov2/        # KiCad v2 回路図・基板アートワーク
        ├── DVC-picov2.kicad_pcb # 基板レイアウト
        ├── DVC-picov2.kicad_pro # KiCad プロジェクトファイル
        ├── DVC-picov2.kicad_sch # 回路図
        └── ...
```

---

## ✨ 主な機能

1. **多彩な通信モード（6チャネル拡張）**
   * **Monitor Only**: 送受信データの監視
   * **Read DMX -> SD**: 受信したDMXデータをCSV形式でSDカードにリアルタイム録画
   * **Write DMX <- SD**: SDカードに記録されたCSVファイルを再生し、DMX信号を出力
   * **Read DMX -> USB**: DMXデータをUSBシリアル経由でPCに送信
   * **Write DMX <- USB**: USB経由で受信したコマンドに基づいてDMX信号を出力
   * **Manual Controller**: 7つの操作キーを使用した、手動でのDMXチャンネル出力制御
2. **5つの画面表示モード**
   * **Waveform Monitor**: DMX各チャネルの値を波形（オシロスコープスタイル）でビジュアル表示
   * **Table Monitor**: DMX 512チャネルの数値をグリッド状に一覧表示
   * **Dashboard**: システムの稼働ステータスや通信統計、診断結果をサマリー表示
   * **Fixture Inspector**: 灯体（フィクスチャ）のチャンネル定義と設定値の確認
   * **Multi-Light Monitor**: 10灯のインテリジェントライトの個別モニターおよびプロファイル調整
3. **対話型フィクスチャ（灯体）エディター**
   * 接続されたDMX機器のチャンネル数や各チャンネルの役割（Dimmer, Red, Green, Blue, Strobe等）を本体キーで自由に変更・カスタム可能
4. **永続データ保存（EEPROMエミュレーション）**
   * 設定したフィクスチャのプロファイルデータをRP2040の内蔵フラッシュメモリに自動で保存・復元

---

## 🛠️ ハードウェア仕様とピン配置

### 主要コンポーネント
* **MCU**: Raspberry Pi Pico / Pico 2 (RP2040 / RP2350)
* **ディスプレイ**: 1.8インチ ST7735 TFTカラー液晶 (160x128)
* **外部ストレージ**: MicroSDカードスロット（ソフトウェアSPI動作）
* **DMXインターフェース**: MAX485トランシーバー搭載

### ピン割り当て (RP2040)
* **TFT ディスプレイ**:
  * `CS`  -> GPIO 22
  * `RST` -> GPIO 21
  * `DC`  -> GPIO 20
  * `LED` -> GPIO 17
* **SDカード (SoftSPI)**:
  * `SCK`  -> GPIO 10
  * `MISO` -> GPIO 11
  * `MOSI` -> GPIO 12
  * `CS`   -> GPIO 13
* **DMX**:
  * `TX`    -> GPIO 0
  * `RX`    -> GPIO 1
  * `RE_DE` -> GPIO 2
* **操作ボタン (Active LOW)**:
  * `SETTING` -> GPIO 9
  * `UP`      -> GPIO 8
  * `DOWN`    -> GPIO 4
  * `LEFT`    -> GPIO 6
  * `RIGHT`   -> GPIO 7
  * `ENTER`   -> GPIO 5
  * `BACK`    -> GPIO 3

---

## ⚙️ 開発環境と依存ライブラリ

ファームウェアをコンパイルするには、Arduino IDEに以下のライブラリを追加してください：

* **Adafruit_GFX** (ディスプレイ描画用)
* **Adafruit_ST7735** (ST7735制御用)
* **SdFat** (SDカードの高速読み書き用)
* **Pico DMX (DmxInput / DmxOutput)** (Earle Philhower版コアのDMX送受信サポート)

*※ボードマネージャーは「Raspberry Pi Pico/RP2040 (Earle Philhower版)」の使用を想定しています。*
