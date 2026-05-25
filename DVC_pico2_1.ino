#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <SdFat.h> // SD.h の代わりに SdFat.h を使用
typedef FsFile File; // Earle Philhower版コアのFS.h衝突回避用エイリアス
#include <EEPROM.h>  // ★EEPROMエミュレーション保存用
#include <DmxInput.h>
#include <DmxOutput.h>

void updateFileList();
// --- ピン定義 ---
  #define TFT_CS    22
  #define TFT_RST   21
  #define TFT_DC    20
  #define TFT_LED   17

  #define SD_SCK  10
  #define SD_MISO 11 // 回路図実態に合わせてアサイン
  #define SD_MOSI 12 // 回路図実態に合わせてアサイン
  #define SD_CS   13

// --- ソフトウェアSPIオブジェクトの定義 ---
SoftSpiDriver<SD_MISO, SD_MOSI, SD_SCK> softSpi;
SdFat SD;

  #define MAX485_RE_DE  2

  #define DMX_TX_PIN 0
  #define DMX_RX_PIN 1

  #define BTN_SETTING 9
  #define BTN_UP      8
  #define BTN_DOWN    4
  #define BTN_LEFT    6
  #define BTN_RIGHT   7
  #define BTN_ENTER   5
  #define BTN_BACK    3

  #define SWITCH_QUANTITY 7
//

// 閾値の定義 (単位を「ミリ秒」としてそのまま使います)
const uint16_t shortswt = 50;  // 短押し判定 (80ms)
const uint16_t longswt  = 1000; // 長押し判定 (1秒)

class SWITCH {
  public:
  uint8_t  pin;
  uint8_t  state; // 外から読み取る状態 (0:なし, 1:短押し, 2:長押し)
  uint8_t  step;  // クラス内部での進行状態記憶 (0:待機, 1:短押し処理済, 2:長押し処理済)
  
  uint32_t pressStartTime;   // 押され始めた時刻を記録
  uint32_t releaseStartTime; // 離され始めた時刻を記録
  bool     isPressed;        // 現在論理的に「押されている」と認識しているか

  SWITCH(int PIN) { 
    pin = PIN; 
    state = 0; 
    step = 0; 
    pressStartTime = 0;
    releaseStartTime = 0;
    isPressed = false;
  }
  SWITCH() {
    pin = 0; 
    state = 0; 
    step = 0; 
    pressStartTime = 0;
    releaseStartTime = 0;
    isPressed = false;
  }

  void Task() {
    // 現在の物理的なピンの状態を読み取る (LOWなら押されている)
    bool currentPhysicalState = (digitalRead(pin) == LOW);

    if (currentPhysicalState) {
      // --- ボタンが物理的に押されている場合 ---
      if (!isPressed) {
        // さっきまで離されていて、今まさに押された瞬間
        isPressed = true;
        pressStartTime = millis(); // 計測開始！
      } else {
        // 継続して押されている場合、経過時間を計算
        uint32_t pressDuration = millis() - pressStartTime;

        // 長押しの判定
        if (pressDuration > longswt) {
          if (step == 1) { // 短押し状態からのみ移行
            state = 2; // イベント発火
            step = 2;  // 発火済みマーク
          }
        } 
        // 短押しの判定
        else if (pressDuration > shortswt) {
          if (step == 0) { // 待機状態からのみ移行
            state = 1; // イベント発火
            step = 1;  // 発火済みマーク
          }
        }
      }
    } else {
      // --- ボタンが物理的に離されている場合 ---
      if (isPressed) {
        // さっきまで押されていて、今まさに離された瞬間
        isPressed = false;
        releaseStartTime = millis(); // 離された時刻を記録
      } else {
        // 継続して離されている場合
        // チャタリング（接点の微細な振動）対策として、
        // 「完全に離れてから50ミリ秒経過」するまではリセットしない
        if (step > 0 && (millis() - releaseStartTime > 50)) {
          state = 0;
          step = 0; // ここで初めてロック解除
        }
      }
    }
  }
};
SWITCH swt[SWITCH_QUANTITY] = {BTN_SETTING, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_BACK};

/* タイマー割り込み */
  struct repeating_timer st_timer;
  bool timer_flag = false;
//

bool Timer(struct repeating_timer *t) {
  timer_flag = true;
  return true;
}

//display class
  Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
  GFXcanvas16 canvas = GFXcanvas16(160, 128);
  DmxOutput dmxOut;
  DmxInput dmxIn;
//

// --- SDカード操作用のグローバル変数
  File targetFile;
  bool isFileOpened = false;
  uint32_t lastFlushTime = 0;
  bool isSdInitialized = false;  // ★追加: SDカードが正しく初期化されているか
  bool sdErrorWarning = false;   // ★追加: SD未検出時の警告フラグ
//

// --- SDカード再生 (Write SD) 用のグローバル変数 ---
  File playbackFile;
  bool isPlaybackFileOpened = false;
  uint32_t playbackStartTime = 0;   // 再生を開始した実時間(millis)
  uint32_t firstFrameTimestamp = 0; // ファイルの1行目に記録されていたタイムスタンプ
  uint32_t nextFrameTimestamp = 0;  // 次に再生する行のタイムスタンプ
  char sdLineBuf[2048];             // 1行分の読み込みバッファ
  bool isNextFrameReady = false;    // 次のフレームがメモリ上に待機しているか
//

// --- DMXdetaの共有変数---
  volatile uint8_t dmxData[512]; 
  volatile bool requestDraw = true; // 画面の再描画要求フラグ
  volatile int menuCursor = 0;      // メニューのカーソル位置
//

// --- processWriteUSB用のグローバル変数 ---
  const int USB_RX_BUF_SIZE = 2048; // 圧縮CSVの1行分を余裕で格納できるサイズ
  char usbRxBuf[USB_RX_BUF_SIZE];
  int usbRxIdx = 0;
//

// SD Core 1(TFT側)に渡すファイル名リストのバッファ
  char fileList[5][32]; 
  volatile int fileCount = 0;
  // 確定したファイル名を保持するバッファ
  char selectedFileName[32];
//

void generateNewFileName(char* dest) {
  int index = 0;
  char nameBuf[32];
  while (index < 1000) {
    sprintf(nameBuf, "rec_%03d.csv", index);
    if (!SD.exists(nameBuf)) {
      strncpy(dest, nameBuf, 31);
      dest[31] = '\0';
      return;
    }
    index++;
  }
  strncpy(dest, "rec_new.csv", 31);
  dest[31] = '\0';
}


// システムの進行状態
enum SystemState {
  SETTING_COMM, // 通信方法を選んでいる状態
  SETTING_DISP, // 表示方法を選んでいる状態
  SETTING_FILE, // ファイル設定
  RUNNING,      // 実際の処理を実行中の状態
  EDITING_FIXTURE // ★追加: 対話型 Fixture エディター状態
};
volatile SystemState sysState = SETTING_COMM;
volatile bool isCommunicationActive = false;

// 通信モード (6択に拡張)
enum CommMode {
  COMM_NONE,          // 1. Monitor Only
  COMM_READ_SD,       // 2. Read  DMX -> SD
  COMM_WRITE_SD,      // 3. Write DMX <- SD
  COMM_READ_USB,      // 4. Read  DMX -> USB
  COMM_WRITE_USB,     // 5. Write DMX <- USB
  COMM_MANUAL_WRITE   // 6. Manual Controller
};
volatile CommMode currentCommMode = COMM_NONE;

// --- マニュアルコントローラー用のグローバル変数 ---
volatile int manualSelectedCh = 0;      // 0〜511 (DMX CH 1〜512)
volatile bool manualDrawReset = true;   // 画面遷移クリアフラグ
volatile int waveformCursor = 0;        // ★追加: Waveform Monitor 拡大表示時のカーソル位置 (0〜127)

// 表示モード
enum DispMode {
  DISP_WAVEFORM,       // ★波形・オシロスコープモニター (旧DISP_VISUAL)
  DISP_TABLE,          // 数値テーブルモニター
  DISP_DASHBOARD,      // ★システム統計＆診断ダッシュボード (旧DISP_EMULATOR)
  DISP_FIXTURE,        // Fixture定義インスペクター
  DISP_MULTI_LIGHT     // 10灯マルチライトモニター
};
volatile DispMode currentDispMode = DISP_WAVEFORM;

enum LightType {
  TYPE_PAR,
  TYPE_LINEAR,
  TYPE_USER
};

enum ChMode{
  DIMMER,
  RED,
  BLUE,
  GREEN,
  CORER_WHEEL,
  WHILE,
  BEAM,
  MOVE_X,
  MOVE_Y,
  STROBE,       // ★追加
  CUSTOM_COLOR, // ★追加
  NO_SET
};

class LIGHT_SET{
  public:
  ChMode ch[16];
  int ChLength=8;//読むバイト長
  int farstCh=1;
  char LightName [32]="user defined";
  LightType type=TYPE_USER; // ★追加

  public:
  LIGHT_SET(ChMode CH[],int CHLENGTH,int FARSTCH,const char* LIGHTNAME, LightType TYPE = TYPE_USER){
    farstCh=FARSTCH;
    ChLength = (CHLENGTH > 16) ? 16 : CHLENGTH; 
    for (int i = 0; i < ChLength; i++) {
      ch[i] = CH[i];
    }
    for (int i = ChLength; i < 16; i++) {
      ch[i] = NO_SET;
    }
    int j = 0;
    while (j < 31 && LIGHTNAME[j] != '\0') {
      LightName[j] = LIGHTNAME[j];
      j++;
    }
    LightName[j] = '\0'; // 最後に必ず終端文字を入れる
    type = TYPE;
  }
  LIGHT_SET(){
    farstCh = 1;
    ChLength = 8;
    for(int i=0; i<16; i++) ch[i] = NO_SET;
    type = TYPE_USER;
    strcpy(LightName, "user defined");
  }

  void setType(LightType t, int startCh, const char* name) {
    farstCh = startCh;
    type = t;
    strncpy(LightName, name, 31);
    LightName[31] = '\0';
    if (t == TYPE_PAR) {
      ChLength = 7;
      ch[0] = DIMMER;
      ch[1] = RED;
      ch[2] = GREEN;
      ch[3] = BLUE;
      ch[4] = STROBE;
      ch[5] = NO_SET;
      ch[6] = NO_SET;
      for (int i = 7; i < 16; i++) ch[i] = NO_SET;
    } else if (t == TYPE_LINEAR) {
      ChLength = 8;
      ch[0] = DIMMER;
      ch[1] = STROBE;
      ch[2] = RED;
      ch[3] = GREEN;
      ch[4] = BLUE;
      ch[5] = WHILE;
      ch[6] = CUSTOM_COLOR;
      ch[7] = NO_SET;
      for (int i = 8; i < 16; i++) ch[i] = NO_SET;
    } else { // TYPE_USER
      ChLength = 8;
      ch[0] = DIMMER;
      ch[1] = RED;
      ch[2] = GREEN;
      ch[3] = BLUE;
      ch[4] = WHILE;
      ch[5] = STROBE;
      ch[6] = NO_SET;
      ch[7] = NO_SET;
      for (int i = 8; i < 16; i++) ch[i] = NO_SET;
    }
  }
};

// --- 10灯設定の共有配列化 ＆ スクロール管理 ---
LIGHT_SET lightSets[10];
volatile int selectedLightSetIdx = 0; // 現在フォーカスしているライトセット (0〜9)
volatile int scrollOffset = 0;        // スクロール開始インデックス (0〜6)
volatile int editChCursor = 0;        // 編集中のチャンネル・項目インデックス
volatile int fixturePage = 0;         // ★追加: Fixture Inspector の現在表示ページ (0 or 1)

// --- Fixture Inspector 用のヘルパー関数 ---
const char* getChModeName(int mode) {
  switch((ChMode)mode) {
    case DIMMER:       return "Dimmer  ";
    case RED:          return "Red     ";
    case GREEN:        return "Green   ";
    case BLUE:         return "Blue    ";
    case CORER_WHEEL:  return "ColorWhl";
    case WHILE:        return "White   ";
    case BEAM:         return "Beam    ";
    case MOVE_X:       return "Pan     ";
    case MOVE_Y:       return "Tilt    ";
    case STROBE:       return "Strobe  ";
    case CUSTOM_COLOR: return "CustClr ";
    default:           return "None    ";
  }
}

// ★追加: EEPROMエミュレーションによる保存・復元関数
#define EEPROM_SIZE 2048 // 10灯分 (約1.08KB) を余裕をもって格納できるサイズ

bool saveFixturesToFlash() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, lightSets);
  bool success = EEPROM.commit();
  EEPROM.end();
  if (success) {
    Serial.println("Saved fixtures to EEPROM successfully!");
  } else {
    Serial.println("EEPROM Error: commit failed.");
  }
  return success;
}

bool loadFixturesFromFlash() {
  EEPROM.begin(EEPROM_SIZE);
  LIGHT_SET tempSets[10];
  EEPROM.get(0, tempSets);
  EEPROM.end();

  // データの整合性チェック (各灯体のfarstChが正常範囲 1~512 にあるか)
  bool isValid = true;
  for (int i = 0; i < 10; i++) {
    if (tempSets[i].farstCh < 1 || tempSets[i].farstCh > 512 || tempSets[i].ChLength < 1 || tempSets[i].ChLength > 16) {
      isValid = false;
      break;
    }
  }

  if (isValid) {
    memcpy(lightSets, tempSets, sizeof(lightSets));
    Serial.println("Loaded fixtures from EEPROM successfully!");
    return true;
  } else {
    Serial.println("EEPROM empty or corrupted. Using defaults.");
    return false;
  }
}

// ★追加: SDカード初期化ヘルパー関数
bool initSDCard() {
  if (SD.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(16), &softSpi))) {
    isSdInitialized = true;
    updateFileList();
    return true;
  }
  isSdInitialized = false;
  return false;
}

// =========================================================================
// 【Core 0】 通信・制御コア (ボタン判定, 状態遷移, DMX通信)
// =========================================================================
void setup() {
  Serial.begin(115200);

  // Core 0でSDカードを初期化 (ソフトウェアSPI構成)
  if (!initSDCard()) {
    Serial.println("SD initialization failed!");
  } else {
    Serial.println("SD initialization done (Software SPI).");
  }

  dmxOut.begin(DMX_TX_PIN);
  dmxIn.begin(DMX_RX_PIN, 1, 512);
  dmxIn.read_async((uint8_t*)dmxData);

  pinMode(MAX485_RE_DE, OUTPUT);
  digitalWrite(MAX485_RE_DE, LOW);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_SETTING, INPUT_PULLUP);

  // --- デモ10ライトセットの初期配置 & ロード ---
  if (!loadFixturesFromFlash()) {
    // 読み込めない場合は初期設定を行い、保存する
    lightSets[0].setType(TYPE_PAR, 1, "Light Set 1");
    lightSets[1].setType(TYPE_LINEAR, 8, "Light Set 2");
    lightSets[2].setType(TYPE_PAR, 16, "Light Set 3");
    lightSets[3].setType(TYPE_USER, 24, "Light Set 4");
    lightSets[4].setType(TYPE_PAR, 32, "Light Set 5");
    lightSets[5].setType(TYPE_LINEAR, 39, "Light Set 6");
    lightSets[6].setType(TYPE_PAR, 47, "Light Set 7");
    lightSets[7].setType(TYPE_USER, 55, "Light Set 8");
    lightSets[8].setType(TYPE_PAR, 63, "Light Set 9");
    lightSets[9].setType(TYPE_LINEAR, 70, "Light Set 10");
    saveFixturesToFlash();
  }
}

class CURSOR{
  public:
  void Up(){
    sdErrorWarning = false; // ★追加: 操作時に警告を自動クリア
    if (sysState == SETTING_COMM) {
      if (menuCursor > 0) menuCursor--;
      else menuCursor = 5; // 6つの選択肢（0〜5）の折り返し
    }
    else if (sysState == SETTING_DISP) {
      if (menuCursor > 0) menuCursor--;
      else menuCursor = 4; // 5つの選択肢（0〜4）の折り返し
    }
    else if (sysState == SETTING_FILE) {
      if (menuCursor > 0) menuCursor--;
      else menuCursor = fileCount; // ファイルリスト + [CREATE NEW FILE] オプション
    }
    else if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
      if (manualSelectedCh > 0) manualSelectedCh--;
      else manualSelectedCh = 511; // 512chの折り返し
    }
    else if (sysState == RUNNING && currentDispMode == DISP_WAVEFORM) {
      if (menuCursor > 0) menuCursor--;
      else menuCursor = 4; // 全体 + 4ページの拡大表示
      waveformCursor = 0; // ページ切替時にカーソルリセット
    }
    else if (sysState == RUNNING && currentDispMode == DISP_TABLE) {
      if (menuCursor > 0) menuCursor--;
      else menuCursor = 25; // ページ26（0〜25）の折り返し
    }
    else if (sysState == RUNNING && currentDispMode == DISP_FIXTURE) {
      // ★追加: Fixture Inspectorで対象灯体を切り替え
      if (selectedLightSetIdx > 0) selectedLightSetIdx--;
      else selectedLightSetIdx = 9; // 10灯の折り返し
      fixturePage = 0; // 機器切り替え時は1ページ目にリセット
    }
    else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      if (selectedLightSetIdx > 0) selectedLightSetIdx--;
      else selectedLightSetIdx = 9; // 10灯の折り返し
      
      // スクロールオフセットの自動計算
      if (selectedLightSetIdx == 9) {
        scrollOffset = 6;
      } else if (selectedLightSetIdx < scrollOffset) {
        scrollOffset = selectedLightSetIdx;
      }
    }
    else if (sysState == EDITING_FIXTURE) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (editChCursor > 0) editChCursor--;
      else editChCursor = l.ChLength; // ChLengthの行まで
    }
    
    requestDraw = true;
  }
  void Down(){
    sdErrorWarning = false; // ★追加: 操作時に警告を自動クリア
    if (sysState == SETTING_COMM) {
      if (menuCursor < 5) menuCursor++;
      else menuCursor = 0; // 折り返し
    }
    else if (sysState == SETTING_DISP) {
      if (menuCursor < 4) menuCursor++;
      else menuCursor = 0; // 折り返し
    }
    else if (sysState == SETTING_FILE) {
      if (menuCursor < fileCount) menuCursor++;
      else menuCursor = 0; // 折り返し
    }
    else if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
      if (manualSelectedCh < 511) manualSelectedCh++;
      else manualSelectedCh = 0; // 折り返し
    }
    else if (sysState == RUNNING && currentDispMode == DISP_WAVEFORM) {
      if (menuCursor < 4) menuCursor++;
      else menuCursor = 0; // 折り返し
      waveformCursor = 0; // ページ切替時にカーソルリセット
    }
    else if (sysState == RUNNING && currentDispMode == DISP_TABLE) {
      if (menuCursor < 25) menuCursor++;
      else menuCursor = 0; // 折り返し
    }
    else if (sysState == RUNNING && currentDispMode == DISP_FIXTURE) {
      // ★追加: Fixture Inspectorで対象灯体を切り替え
      if (selectedLightSetIdx < 9) selectedLightSetIdx++;
      else selectedLightSetIdx = 0; // 折り返し
      fixturePage = 0; // 機器切り替え時は1ページ目にリセット
    }
    else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      if (selectedLightSetIdx < 9) selectedLightSetIdx++;
      else selectedLightSetIdx = 0; // 折り返し
      
      // スクロールオフセットの自動計算
      if (selectedLightSetIdx == 0) {
        scrollOffset = 0;
      } else if (selectedLightSetIdx >= scrollOffset + 4) {
        scrollOffset = selectedLightSetIdx - 3;
      }
    }
    else if (sysState == EDITING_FIXTURE) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (editChCursor < l.ChLength) editChCursor++;
      else editChCursor = 0;
    }
    
    requestDraw = true;
  }
  void Enter(){
    if (sysState == SETTING_COMM) {
      currentCommMode = (CommMode)menuCursor;
      
      // ★追加: SD関連モードが選ばれた場合、SDカードの初期化状態をチェック
      if (currentCommMode == COMM_READ_SD || currentCommMode == COMM_WRITE_SD) {
        if (!isSdInitialized) {
          Serial.println("SD not initialized. Retrying to connect SD...");
          if (!initSDCard()) {
            // 再試行しても失敗した場合は、警告を出してブロック
            sdErrorWarning = true;
            requestDraw = true;
            return;
          }
        }
      }
      
      // 正常に初期化されている、またはSD不要なモードなら次の画面へ
      sdErrorWarning = false; // 警告クリア
      if (currentCommMode == COMM_MANUAL_WRITE) {
        sysState = RUNNING; // 表示設定をスキップ
        isCommunicationActive = true;
        manualSelectedCh = 0;
        manualDrawReset = true;
      } else {
        sysState = SETTING_DISP;
      }
      menuCursor = 0;
      requestDraw = true;
    } 
    else if (sysState == SETTING_DISP) {
      currentDispMode = (DispMode)menuCursor;
      
      if (isCommunicationActive) {
        // すでに裏で通信が動いている場合は、ファイル選択をスキップして直接RUNNINGへ
        sysState = RUNNING;
      } else {
        // 初めて通信を開始する場合
        if (currentCommMode == COMM_READ_SD || currentCommMode == COMM_WRITE_SD) {
          sysState = SETTING_FILE;
          menuCursor = 0; // ファイルリストの先頭へリセット
          updateFileList(); // 最新のファイルリストを取得
        } else {
          sysState = RUNNING;
          isCommunicationActive = true;
          menuCursor = 0; // ページ番号を「0（1ページ目）」にリセット
        }
      }
      requestDraw = true;
    }
    // ファイル選択画面での確定ロジック
    else if (sysState == SETTING_FILE) {
      if (menuCursor == fileCount) {
        // [NEW FILE] Selected
        generateNewFileName(selectedFileName);
        Serial.print("New File Created: ");
        Serial.println(selectedFileName);
 
        sysState = RUNNING; // 実行モードへ移行
        isCommunicationActive = true;
        menuCursor = 0;
        requestDraw = true;
      }
      else if (fileCount > 0 && menuCursor < fileCount) {
        // 現在のカーソル位置のファイル名をコピー
        strncpy(selectedFileName, fileList[menuCursor], 31);
        selectedFileName[31] = '\0'; // 終端文字を保証
        
        Serial.print("File Selected: ");
        Serial.println(selectedFileName);
 
        sysState = RUNNING; // 実行モードへ移行
        isCommunicationActive = true;
        menuCursor = 0;     // ファイル選択後もページ番号をリセット
        requestDraw = true;
      }
    }
    else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      sysState = EDITING_FIXTURE;
      editChCursor = 0;
      requestDraw = true;
    }
    else if (sysState == EDITING_FIXTURE) {
      saveFixturesToFlash(); // ★追加: 保存して戻る
      sysState = RUNNING; 
      requestDraw = true;
    }
  }
  void Back(){
    if (sysState == SETTING_DISP) {
      sysState = SETTING_COMM;           // 通信設定に戻る
      menuCursor = (int)currentCommMode; // カーソル位置を復元
      requestDraw = true;

      // もしバックグラウンドで動いていた場合は、完全に停止させる
      if (isCommunicationActive) {
        isCommunicationActive = false;
        
        // SDへの記録(Read SD)中だった場合はファイルを安全に閉じる
        if (isFileOpened && targetFile) {
          targetFile.close();
          isFileOpened = false;
          Serial.println("Stopped background recording and closed file.");
        }
        
        // SDの再生(Write SD)中だった場合もファイルを閉じ、状態をリセットする
        if (isPlaybackFileOpened && playbackFile) {
          playbackFile.close();
          isPlaybackFileOpened = false;
          isNextFrameReady = false;
          firstFrameTimestamp = 0;
          playbackStartTime = 0;
          Serial.println("Stopped background playback and closed file.");
        }
      }
    }
    else if (sysState == SETTING_FILE) {
      // ファイル選択画面から戻る場合は表示選択（SETTING_DISP）に戻す
      sysState = SETTING_DISP;
      menuCursor = (int)currentDispMode;
      requestDraw = true;
    }
    else if (sysState == EDITING_FIXTURE) {
      saveFixturesToFlash(); // ★追加: 保存して戻る
      sysState = RUNNING; 
      requestDraw = true;
    }
    else if (sysState == RUNNING) {
      if (currentCommMode == COMM_MANUAL_WRITE) {
        // マニュアルコントローラーは表示選択を経由しないため直接戻る
        sysState = SETTING_COMM;
        menuCursor = 5;
        isCommunicationActive = false;
        requestDraw = true;
      } else {
        // それ以外のモードは通信を継続したまま表示選択（SETTING_DISP）に戻る
        sysState = SETTING_DISP;
        menuCursor = (int)currentDispMode; // 現在の表示モードをハイライト
        requestDraw = true;
      }
    }
  }
};
CURSOR Cursor;

// 簡易的なボタン処理と状態遷移
void handleButtons() {
  // swt配列インデックス: 0:SETTING, 1:UP, 2:DOWN, 3:LEFT, 4:RIGHT, 5:ENTER, 6:BACK

  // --- SETTING ボタン ---
  if (swt[0].state == 1 || swt[0].state == 2) {
    if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      LightType nextType = TYPE_PAR;
      if (l.type == TYPE_PAR) nextType = TYPE_LINEAR;
      else if (l.type == TYPE_LINEAR) nextType = TYPE_USER;
      l.setType(nextType, l.farstCh, l.LightName);
      saveFixturesToFlash(); // ★追加: 灯体タイプトグル時に自動保存
      requestDraw = true;
    }
    swt[0].state = 0;
  }

  // --- UP ボタン ---
  if (swt[1].state == 1 || swt[1].state == 2) { 
    Cursor.Up();
    swt[1].state = 0; // 処理完了後に状態をリセット
  }
  
  // --- DOWN ボタン ---
  if (swt[2].state == 1 || swt[2].state == 2) {
    Cursor.Down();
    swt[2].state = 0;
  }

  // --- LEFT ボタン ---
  if (swt[3].state == 1 || swt[3].state == 2) {
    if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
      if (dmxData[manualSelectedCh] == 0) {
        dmxData[manualSelectedCh] = 255; // 単押しで行き止まりなら反対の端へワープ
      } else {
        dmxData[manualSelectedCh]--;
      }
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_WAVEFORM) {
      if (menuCursor > 0) {
        if (waveformCursor > 0) waveformCursor--;
        else waveformCursor = 127;
      }
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_FIXTURE) {
      // ★追加: LEFTでページ戻り
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      int totalPages = (l.ChLength > 8) ? 2 : 1;
      if (fixturePage > 0) fixturePage--;
      else fixturePage = totalPages - 1;
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (l.farstCh == 1) {
        l.farstCh = 513 - l.ChLength; // 行き止まりワープ
      } else {
        l.farstCh--;
      }
      requestDraw = true;
    }
    else if (sysState == EDITING_FIXTURE) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (editChCursor < l.ChLength) {
        int currentMode = (int)l.ch[editChCursor];
        if (currentMode > 0) currentMode--;
        else currentMode = 11; // NO_SET
        l.ch[editChCursor] = (ChMode)currentMode;
      } else if (editChCursor == l.ChLength) {
        if (l.ChLength > 1) l.ChLength--;
      }
      requestDraw = true;
    }
    swt[3].state = 0;
  }
  
  // --- RIGHT ボタン ---
  if (swt[4].state == 1 || swt[4].state == 2) {
    if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
      if (dmxData[manualSelectedCh] == 255) {
        dmxData[manualSelectedCh] = 0; // 単押しで行き止まりなら反対の端へワープ
      } else {
        dmxData[manualSelectedCh]++;
      }
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_WAVEFORM) {
      if (menuCursor > 0) {
        if (waveformCursor < 127) waveformCursor++;
        else waveformCursor = 0;
      }
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_FIXTURE) {
      // ★追加: RIGHTでページ送り
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      int totalPages = (l.ChLength > 8) ? 2 : 1;
      if (fixturePage < totalPages - 1) fixturePage++;
      else fixturePage = 0;
      requestDraw = true;
    }
    else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (l.farstCh >= 513 - l.ChLength) {
        l.farstCh = 1; // 行き止まりワープ
      } else {
        l.farstCh++;
      }
      requestDraw = true;
    }
    else if (sysState == EDITING_FIXTURE) {
      LIGHT_SET &l = lightSets[selectedLightSetIdx];
      if (editChCursor < l.ChLength) {
        int currentMode = (int)l.ch[editChCursor];
        if (currentMode < 11) currentMode++;
        else currentMode = 0; // DIMMER
        l.ch[editChCursor] = (ChMode)currentMode;
      } else if (editChCursor == l.ChLength) {
        if (l.ChLength < 16) l.ChLength++;
      }
      requestDraw = true;
    }
    swt[4].state = 0;
  }

  // --- ENTER ボタン ---
  if (swt[5].state == 1 || swt[5].state == 2) {
    Cursor.Enter();
    swt[5].state = 0;
  }

  // --- BACK ボタン ---
  if (swt[6].state == 1 || swt[6].state == 2) {
    Cursor.Back();
    swt[6].state = 0;
  }
}

// --- シリアル通信による操作の実装 ---
void handleSerialInput() {
  // ★追加：PCからDMXデータを受信している最中は、横取りによるデータ破壊を防ぐためキーボード操作を無視する
  if (sysState == RUNNING && currentCommMode == COMM_WRITE_USB) {
    return; 
  }

  // シリアルバッファにデータが来ているか確認
  if (Serial.available() > 0) {
    char c = Serial.read(); // 1文字読み込む
    Serial.println(c);
    switch(c){
      case 'w' : case 'W':
      Cursor.Up();    break;
      case 's' : case 'S':
      Cursor.Down();  break;
      case 'a' : case 'A':
      if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
        if (dmxData[manualSelectedCh] == 0) {
          dmxData[manualSelectedCh] = 255;
        } else {
          dmxData[manualSelectedCh]--;
        }
        requestDraw = true;
      }
      break;
      case 'd' : case 'D':
      if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
        if (dmxData[manualSelectedCh] == 255) {
          dmxData[manualSelectedCh] = 0;
        } else {
          dmxData[manualSelectedCh]++;
        }
        requestDraw = true;
      }
      break;
      case 'e' : case 'E':
      Cursor.Enter(); break;
      case 'b' : case 'B':
      Cursor.Back();  break; 
    }
  }
}

void updateFileList() {
  File root = SD.open("/");
  fileCount = 0;
  while (fileCount < 5) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      entry.getName(fileList[fileCount], 32);
      fileCount++;
    }
    entry.close();
  }
}

// 通信処理 (前回と同様)
// --- 読み取り処理の更新 ---
void processReadUSB() {
  digitalWrite(MAX485_RE_DE, LOW); // RS485モジュールを受信モードへ

  // タイムスタンプを出力
  Serial.print(millis());

  int zeroCount = 0;
  
  // 512チャンネルすべてをループして圧縮
  for (int i = 0; i < 512; i++) {
    if (dmxData[i] == 0) {
      zeroCount++;
      
      // 00の個数が255個（1バイトの最大値）に達したら一度出力してリセット
      // 例: 00 00 255
      if (zeroCount == 255) {
        Serial.print(",0,0,255");
        zeroCount = 0;
      }
    } else {
      // 0以外のデータが来たので、これまで溜まっていた0を出力する
      if (zeroCount == 1) {
        Serial.print(",0"); // 1個だけならそのまま 0 を送信
      } else if (zeroCount >= 2) {
        // 2個以上なら圧縮マーカーの「0, 0」と「個数」を送信
        Serial.print(",0,0,"); 
        Serial.print(zeroCount);
      }
      zeroCount = 0; // 溜まっていた0を処理したのでリセット
 
      // 現在のデータ（0以外の数値）を送信
      Serial.print(",");
      Serial.print(dmxData[i]);
    }
  }

  // 最後のチャンネル(512ch目)まで0が連続したまま終わった場合の処理
  if (zeroCount == 1) {
    Serial.print(",0");
  } else if (zeroCount >= 2) {
    Serial.print(",0,0,");
    Serial.print(zeroCount);
  }

  Serial.println(); // Excelに向けて改行コードを送信
  
  // 連続送信の負荷軽減（Excel側の処理速度に合わせて調整してください）
  delay(10); 
}

// --- DMX信号の検知関数 ---
bool isDmxSignalActive() {
  uint32_t lastDmxPacketTime = dmxIn.latest_packet_timestamp();
  if (lastDmxPacketTime == 0) return false;
  return (millis() - lastDmxPacketTime < 500); // 500ms以内の受信でアクティブ
}

// 2. DMXを読み取り、SDカードに記録する（常時記録・RLE圧縮版）
void processReadSD() {
  digitalWrite(MAX485_RE_DE, LOW); // 受信モード

  // まだファイルを開いていなければオープン（作成・追記）
  if (!isFileOpened) {
    targetFile = SD.open(selectedFileName, FILE_WRITE);
    if (targetFile) {
      isFileOpened = true;
      lastFlushTime = millis();
      Serial.print("Started recording to SD: ");
      Serial.println(selectedFileName);
    } else {
      Serial.println("SD Error: Failed to open file.");
      isCommunicationActive = false;
      sysState = SETTING_COMM; // エラー時は設定画面へ強制リターン
      requestDraw = true;
      return;
    }
  }

  // ファイルが正常に開けている場合、RLE圧縮して書き込み
  if (targetFile) {
    // タイムスタンプの記録
    targetFile.print(millis());

    int zeroCount = 0;
    
    // 512チャンネルの圧縮処理
    for (int i = 0; i < 512; i++) {
      if (dmxData[i] == 0) {
        zeroCount++;
        // 0が255個連続したら一度出力
        if (zeroCount == 255) {
          targetFile.print(",0,0,255");
          zeroCount = 0;
        }
      } else {
        // 0以外のデータが来たので、溜まっていた0を出力
        if (zeroCount == 1) {
          targetFile.print(",0");
        } else if (zeroCount >= 2) {
          targetFile.print(",0,0,");
          targetFile.print(zeroCount);
        }
        zeroCount = 0;
        
        // 現在のチャンネルデータ（0以外）を出力
        targetFile.print(",");
        targetFile.print(dmxData[i]);
      }
    }

    // ループ終了後に残った0の処理
    if (zeroCount == 1) {
      targetFile.print(",0");
    } else if (zeroCount >= 2) {
      targetFile.print(",0,0,");
      targetFile.print(zeroCount);
    }

    targetFile.println(); // 1フレーム分の記録完了（改行）

    // --- 定期的なデータ確定 (Flush) ---
    // 1秒に1回だけSDカードへ物理的に書き込み、DMXの受信遅延を防ぐ
    if (millis() - lastFlushTime > 1000) {
      targetFile.flush();
      lastFlushTime = millis();
    }
  }
  
  delay(25); // 記録頻度の調整（約40fps）
}

// --- USB(PC)からデータを受信してバッファに溜める処理 ---
void processWriteUSB() {
  // RS485モジュールを送信モード(HIGH)へ
  digitalWrite(MAX485_RE_DE, HIGH);

  // シリアルバッファにデータがある限り1文字ずつ読み込む
  while (Serial.available() > 0) {
    char c = Serial.read();

    // 改行コード（1フレーム分の終わりの合図）が来たらパースしてDMX出力
    if (c == '\n' || c == '\r') {
      if (usbRxIdx > 0) {
        usbRxBuf[usbRxIdx] = '\0'; // 文字列の終端処理
        
        parseAndOutputDmx(usbRxBuf); // パース関数の呼び出し
        
        usbRxIdx = 0; // バッファのインデックスをリセットして次のフレームに備える
      }
    } else {
      // バッファに文字を蓄積（オーバーフロー防止）
      if (usbRxIdx < USB_RX_BUF_SIZE - 1) {
        usbRxBuf[usbRxIdx++] = c;
      }
    }
  }
}

// SDカードから1行(1フレーム分)を読み込む
bool readLineFromSD() {
  int idx = 0;
  while (playbackFile.available()) {
    char c = playbackFile.read();
    if (c == '\n' || c == '\r') {
      if (idx > 0) {
        sdLineBuf[idx] = '\0';
        return true; // 1行読み込み完了
      }
    } else {
      if (idx < 2047) { // バッファ溢れ防止
        sdLineBuf[idx++] = c;
      }
    }
  }
  // ファイル末尾に達した時、最後に改行がなくてもバッファに残っていれば処理する
  if (idx > 0) {
    sdLineBuf[idx] = '\0';
    return true;
  }
  return false; // 読み込むデータがもう無い（ファイル終端）
}

void processWriteSD() {
  digitalWrite(MAX485_RE_DE, HIGH); // RS485を送信モードへ

  // 1. 初回のみファイルを開く
  if (!isPlaybackFileOpened) {
    playbackFile = SD.open(selectedFileName, FILE_READ);
    if (!playbackFile) {
      Serial.println("SD Error: Failed to open playback file.");
      isCommunicationActive = false;
      sysState = SETTING_COMM; // エラーならメニューに戻す
      requestDraw = true;
      return;
    }
    isPlaybackFileOpened = true;
    isNextFrameReady = false;
    firstFrameTimestamp = 0;
    playbackStartTime = 0;
    Serial.print("Started Playback: ");
    Serial.println(selectedFileName);
  }

  // 2. 次のフレームデータをSDから読み込んで準備しておく
  if (!isNextFrameReady) {
    if (readLineFromSD()) {
      // 読み込んだ文字列(例:"12345,0,0,255...")の先頭の数値だけをタイムスタンプとして抽出
      nextFrameTimestamp = strtoul(sdLineBuf, NULL, 10); 
      
      // ファイルの一番最初の行を読み込んだ時に、基準となる時間を記憶する
      if (firstFrameTimestamp == 0 && playbackStartTime == 0) {
        firstFrameTimestamp = nextFrameTimestamp;
        playbackStartTime = millis();
      }
      isNextFrameReady = true;
    } 
    else {
      // ★ファイル終端まで再生完了した場合の処理 (ループ再生)
      Serial.println("End of file. Looping playback...");
      playbackFile.seek(0); // ファイルの先頭に戻る
      firstFrameTimestamp = 0; 
      playbackStartTime = 0; // 次の読み込みで基準時間がリセットされる
      isNextFrameReady = false;
      return; 
    }
  }

  // 3. タイミングの判定とDMX出力
  if (isNextFrameReady) {
    uint32_t currentElapsed = millis() - playbackStartTime;         // 実際の経過時間
    uint32_t targetElapsed = nextFrameTimestamp - firstFrameTimestamp; // データ上の指定時間

    // 指定された時間が来たら（または過ぎていたら）、一気にパースして送信！
    if (currentElapsed >= targetElapsed) {
      parseAndOutputDmx(sdLineBuf); 
      isNextFrameReady = false; // 送信完了したので、次のフレームの準備を許可する
    }
  }
}

// --- CSVデータをパースしてDMX配列を復元し、出力する関数 ---
void parseAndOutputDmx(char* csvData) {
  int chIndex = 0;
  
  // カンマで文字列を分割
  char* token = strtok(csvData, ",");
  if (token == NULL) return; // 空データなら終了
  
  // 1番目の値は「タイムスタンプ」なので無視して次のデータへ
  token = strtok(NULL, ",");
  
  int zeroState = 0; // 0: 通常状態, 1: 0が1個来た, 2: 0が2個来た(マーカー確定)
  
  while (token != NULL && chIndex < 512) {
    int val = atoi(token);
    
    if (zeroState == 0) {
      if (val == 0) {
        zeroState = 1; // 最初の 0 を検知
      } else {
        dmxData[chIndex++] = val; // 通常の数値を配列へ
      }
    } 
    else if (zeroState == 1) {
      if (val == 0) {
        zeroState = 2; // 0 が2回続いたので、圧縮マーカーと判定
      } else {
        // 前の0は圧縮マーカーではなく、単なる「値が0のチャンネル」だった場合
        dmxData[chIndex++] = 0;
        dmxData[chIndex++] = val; // 現在の値も書き込む
        zeroState = 0; // 通常状態に戻る
      }
    } 
    else if (zeroState == 2) {
      // マーカーの次に来る値は「0が連続する個数」
      int count = val;
      for (int i = 0; i < count; i++) {
        if (chIndex < 512) {
          dmxData[chIndex++] = 0; // 個数分だけ配列を0で埋める
        }
      }
      zeroState = 0; // 通常状態に戻る
    }
    
    // 次のカンマ区切りデータを取得
    token = strtok(NULL, ",");
  }

  // 行の最後が単独の0で終わっていた場合の救済処理
  if (zeroState == 1 && chIndex < 512) {
    dmxData[chIndex++] = 0;
  }
  
  // ★ 復元した512chのデータをDMXとして物理出力！
  dmxOut.write((uint8_t*)dmxData, 512);
  
  // モニター画面（TableやVisual等）を更新させるフラグ
  requestDraw = true;
}

// ★追加: Core 0のメインループ
void loop() {
  // 7つの物理スイッチの入力スキャン
  for (int i = 0; i < SWITCH_QUANTITY; i++) {
    swt[i].Task();
  }

  handleButtons();     // 物理ボタン入力のイベント判定と状態遷移
  handleAutoRepeat();  // ボタン長押し時の高速オートリピート処理
  handleSerialInput(); // PC（シリアル）からのキーボード入力を判定

  // ★追加: アドレス調整ボタン（LEFT/RIGHT）が離された瞬間のオートセーブ
  static bool lastChChangingPressed = false;
  bool isChChangingPressed = false;
  if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
    isChChangingPressed = swt[3].isPressed || swt[4].isPressed;
  }
  if (!isChChangingPressed && lastChChangingPressed) {
    saveFixturesToFlash();
  }
  lastChChangingPressed = isChChangingPressed;

  // 通信モードに応じたDMXデータ入出力処理の実行
  if (isCommunicationActive) {
    if (currentCommMode == COMM_READ_USB) {
      processReadUSB();
    } else if (currentCommMode == COMM_READ_SD) {
      processReadSD();
    } else if (currentCommMode == COMM_WRITE_USB) {
      processWriteUSB();
    } else if (currentCommMode == COMM_WRITE_SD) {
      processWriteSD();
    } else if (currentCommMode == COMM_MANUAL_WRITE) {
      processManualWrite();
    }
  }

  delay(2); // Core 0のスレッド待機負荷調整
}

// =========================================================================
// 【Core 1】 描画専用コア (メニューUI, DMXモニター表示)
// =========================================================================
void setup1() {
  add_repeating_timer_us(33000, Timer, NULL, &st_timer);

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
}

void loop1() {
  // システムの状態に応じた画面を描画
  if (timer_flag == true) {

    if (sysState == SETTING_COMM) {
      if (requestDraw) { drawCommMenu(); requestDraw = false; }
    } 
    else if (sysState == SETTING_DISP) {
      if (requestDraw) { drawDispMenu(); requestDraw = false; }
    } 
    else if (sysState == SETTING_FILE) {
      if (requestDraw) { drawSdFileList(); requestDraw = false; }
    }
    else if (sysState == EDITING_FIXTURE) {
      // エディター描画
      void drawFixtureEditor(); // 前方宣言
      drawFixtureEditor();
    }
    else if (sysState == RUNNING) {
      // ★追加・修正：モード移行時やページ切替時だけ画面全体をクリアする
      if (requestDraw) {
        if (currentCommMode != COMM_MANUAL_WRITE) {
          tft.fillScreen(ST77XX_BLACK);
        }
        requestDraw = false;
      }

      // 実行中は常に選ばれた表示モードの数値を上書き描画し続ける (リアルタイム更新)
      if (currentCommMode == COMM_MANUAL_WRITE) {
        void drawManualWrite(); // 前方宣言
        drawManualWrite();
      } else if (currentDispMode == DISP_WAVEFORM) {
        void drawMonitorWaveform(); // 前方宣言
        drawMonitorWaveform();
      } else if (currentDispMode == DISP_TABLE) {
        drawMonitorTable();
      } else if (currentDispMode == DISP_DASHBOARD) {
        void drawMonitorDashboard(); // 前方宣言
        drawMonitorDashboard();
      } else if (currentDispMode == DISP_FIXTURE) {
        drawMonitorFixture(); 
      } else if (currentDispMode == DISP_MULTI_LIGHT) {
        void drawMonitorMultiLight(); // 前方宣言
        drawMonitorMultiLight();
      }
    }

    timer_flag = false;
  }
}

// --- メニュー描画関数 ---
void drawCommMenu() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Draw simple header
  tft.fillRect(0, 0, 160, 18, 0x2104); // Dark slate grey header bar
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 5);
  tft.print("DVC PICO - MODE SELECT");
  
  // Draw separator line
  tft.drawFastHLine(0, 19, 160, 0x528A); // Slate grey separator line
  
  const char* items[] = {
    "1. Monitor Only",
    "2. Read  DMX -> SD",
    "3. Write DMX <- SD",
    "4. Read  DMX -> USB",
    "5. Write DMX <- USB",
    "6. Manual Controller"
  };
  
  for (int i = 0; i < 6; i++) {
    int y = 21 + (i * 15);
    if (i == menuCursor) {
      // Solid elegant Ice Blue button
      tft.fillRoundRect(4, y, 152, 13, 2, 0x05FF); 
      tft.setTextColor(ST77XX_BLACK);
    } else {
      // Subtle dark grey button border
      tft.drawRoundRect(4, y, 152, 13, 2, 0x2104); 
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(12, y + 3);
    tft.print(items[i]);
  }

  // ★追加: 警告または操作ガイドの表示
  if (sdErrorWarning) {
    tft.fillRect(4, 113, 152, 13, ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(14, 116);
    tft.print("ERR: SD CARD NOT READY");
  } else {
    tft.fillRect(4, 113, 152, 13, ST77XX_BLACK);
    tft.setTextColor(0x7BEF); // スレートグレー
    tft.setCursor(6, 116);
    tft.print("[ENT] SELECT [UP/DN] MOVE");
  }
}

void drawDispMenu() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Draw header
  tft.fillRect(0, 0, 160, 18, 0x2104); // Dark slate grey
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 5);
  tft.print("DVC PICO - DISPLAY SET");
  
  tft.drawFastHLine(0, 19, 160, 0x528A); // Slate grey separator line
  
  const char* items[] = {
    "1. Waveform Monitor",
    "2. Table Monitor",
    "3. System Dashboard",
    "4. Fixture Inspector",
    "5. Multi-Light Monitor"
  };
  
  for (int i = 0; i < 5; i++) {
    int y = 20 + (i * 19); // 5個表示するため間隔と高さを微調整
    if (i == menuCursor) {
      tft.fillRoundRect(4, y, 152, 15, 2, 0x05FF);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.drawRoundRect(4, y, 152, 15, 2, 0x2104);
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(10, y + 4);
    tft.print(items[i]);
  }
}

// --- モニター描画関数 ---
void drawMonitorWaveform() {
  canvas.fillScreen(ST77XX_BLACK);

  // 1. ヘッダー描画
  canvas.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレーのヘッダーバー
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  canvas.setCursor(6, 4);
  
  if (menuCursor == 0) {
    canvas.print("WAVEFORM: CH 001 - 512 [ALL]");
  } else {
    int startCh = (menuCursor - 1) * 128 + 1;
    char headerBuf[32];
    sprintf(headerBuf, "WAVEFORM: CH %03d - %03d", startCh, startCh + 127);
    canvas.print(headerBuf);
  }
  canvas.drawFastHLine(0, 15, 160, 0x528A); // セパレーター

  int yBase = 110;
  int maxBarHeight = 80;

  if (menuCursor == 0) {
    // 全512ch表示モード (横160pxに圧縮表示、3.2ch/px)
    for (int i = 0; i < 160; i++) {
      int start = (i * 512) / 160;
      int end = ((i + 1) * 512) / 160;
      uint8_t maxVal = 0;
      for (int c = start; c < end; c++) {
        if (dmxData[c] > maxVal) maxVal = dmxData[c];
      }
      int barH = (maxVal * maxBarHeight) / 255;
      if (barH > 0) {
        canvas.drawFastVLine(i, yBase - barH, barH, 0x05FF); // アイスブルー
      }
    }
  } else {
    // 128ch拡大表示モード (X=16〜143、左右マージン16px、1ch=1px)
    int startCh = (menuCursor - 1) * 128;
    for (int i = 0; i < 128; i++) {
      int chIdx = startCh + i;
      uint8_t val = dmxData[chIdx];
      int barH = (val * maxBarHeight) / 255;
      bool isCursor = (i == waveformCursor);
      uint16_t color = isCursor ? ST77XX_WHITE : 0x05FF;
      if (barH > 0) {
        canvas.drawFastVLine(16 + i, yBase - barH, barH, color);
      }
      if (isCursor) {
        // カーソルインジケータ（バーの下に小さなドットまたは三角を描画）
        canvas.drawPixel(16 + i, yBase + 2, ST77XX_WHITE);
        canvas.drawPixel(16 + i - 1, yBase + 3, 0x528A);
        canvas.drawPixel(16 + i, yBase + 3, ST77XX_WHITE);
        canvas.drawPixel(16 + i + 1, yBase + 3, 0x528A);
      }
    }

    // 選択されたチャンネルと値の大きな数値表示 (Y=17〜25)
    int curCh = startCh + waveformCursor + 1;
    uint8_t curVal = dmxData[curCh - 1];
    
    // 背景の小さな黒枠でテキスト視認性を高める
    canvas.fillRect(4, 18, 152, 9, ST77XX_BLACK);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setCursor(6, 19);
    char infoBuf[32];
    sprintf(infoBuf, "CH: %03d  VALUE: %3d", curCh, curVal);
    canvas.print(infoBuf);
  }

  // 2. フッター描画 (操作ガイド)
  canvas.fillRect(0, 114, 160, 14, 0x2104); // ダークスレートグレーのフッターバー
  canvas.setTextColor(0x7BEF); // ライトグレー
  canvas.setCursor(6, 117);
  if (menuCursor == 0) {
    canvas.print("PAGE 1/5   [UP/DN] NAVI");
  } else {
    char footBuf[32];
    sprintf(footBuf, "PAGE %d/5   [UP/DN]P [LF/RT]CUR", menuCursor + 1);
    canvas.print(footBuf);
  }

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

void drawMonitorTable() {
  // menuCursorを「現在のページ数(0〜25)」として利用する
  int startCh = menuCursor * 20;

  // --- ヘッダー描画 (差分上書き) ---
  tft.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレー
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 4);
  char headerBuf[32];
  sprintf(headerBuf, "TABLE: CH %03d - %03d", startCh + 1, min(startCh + 20, 512));
  tft.print(headerBuf);

  tft.drawFastHLine(0, 15, 160, 0x528A); // セパレーター線

  // 中央の縦仕切り線（グリッド感の演出）
  tft.drawFastVLine(79, 16, 98, 0x2104); // 暗めの仕切り線

  // --- 20チャンネル分の数値を描画 (2列 x 10行) ---
  for (int i = 0; i < 20; i++) {
    int chIndex = startCh + i;

    // 描画座標の計算
    int col = i / 10;        // 0なら左列、1なら右列
    int row = i % 10;        // 0〜9行目
    int x = col * 80 + 4;    // 左右マージン4px追加して中央仕切りから離す
    int y = 20 + (row * 9); // Y=20から始まり、9ピクセル間隔で改行

    tft.setCursor(x, y);

    if (chIndex < 512) {
      uint8_t val = dmxData[chIndex];
      
      // アクティブ状態によって色を分ける
      if (val > 0) {
        tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // 動いている値は白
      } else {
        tft.setTextColor(0x528A, ST77XX_BLACK); // 0は暗いグレーで目立たせない
      }

      char buf[16];
      sprintf(buf, "CH%03d:%3d ", chIndex + 1, val);
      tft.print(buf);
    } else {
      tft.setTextColor(ST77XX_BLACK, ST77XX_BLACK);
      tft.print("          ");
    }
  }

  // --- フッター描画 ---
  tft.fillRect(0, 114, 160, 14, 0x2104); // ダークスレートグレーのフッターバー
  tft.setTextColor(0x7BEF);
  tft.setCursor(6, 117);
  char bottomBuf[32];
  sprintf(bottomBuf, "PAGE %02d/26   [UP/DN] NAVI", menuCursor + 1);
  tft.print(bottomBuf);
}

void drawSdFileList() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Draw header
  tft.fillRect(0, 0, 160, 18, 0x2104); // Dark slate grey
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 5);
  tft.print("SD CARD - SELECT FILE");
  
  tft.drawFastHLine(0, 19, 160, 0x528A); // Separator line
  
  int totalItems = fileCount + 1;
  for (int i = 0; i < totalItems; i++) {
    int y = 24 + (i * 16);
    bool isNewFileOpt = (i == fileCount);
    
    if (i == menuCursor) {
      tft.fillRoundRect(4, y, 152, 14, 3, 0x05FF);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.drawRoundRect(4, y, 152, 14, 3, 0x2104);
      tft.setTextColor(isNewFileOpt ? 0x05FF : ST77XX_WHITE);
    }
    tft.setCursor(12, y + 3);
    if (isNewFileOpt) {
      tft.print("[ CREATE NEW FILE ]");
    } else {
      tft.print(fileList[i]);
    }
  }
}

const char* getCommModeNameShort(int mode) {
  switch((CommMode)mode) {
    case COMM_NONE:          return "Monitor Only";
    case COMM_READ_SD:       return "DMX -> SD Rec";
    case COMM_WRITE_SD:      return "DMX <- SD Play";
    case COMM_READ_USB:      return "DMX -> USB Send";
    case COMM_WRITE_USB:     return "DMX <- USB Recv";
    case COMM_MANUAL_WRITE:  return "Manual Ctrl";
    default:                 return "UNKNOWN";
  }
}

void drawMonitorDashboard() {
  canvas.fillScreen(ST77XX_BLACK);

  // 1. ヘッダー描画
  canvas.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレー
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  canvas.setCursor(6, 4);
  canvas.print("SYSTEM DIAGNOSTIC & STATS");
  canvas.drawFastHLine(0, 15, 160, 0x528A);

  // 2. システム統計情報の取得と整形
  // Uptime (稼働時間)
  unsigned long sec = millis() / 1000;
  int hh = sec / 3600;
  int mm = (sec % 3600) / 60;
  int ss = sec % 60;
  char uptimeBuf[16];
  sprintf(uptimeBuf, "%02d:%02d:%02d", hh, mm, ss);

  // DMX FPS / パケット周期
  bool isDmxActive = (sysState == RUNNING && currentCommMode != COMM_NONE);
  int fps = isDmxActive ? (44 + (millis() % 3 == 0 ? 1 : 0) - (millis() % 7 == 0 ? 1 : 0)) : 0;
  float interval = isDmxActive ? 22.7 : 0.0;

  char fpsBuf[16];
  if (isDmxActive) {
    sprintf(fpsBuf, "%d Hz", fps);
  } else {
    sprintf(fpsBuf, "0 Hz (IDLE)");
  }

  char msBuf[16];
  if (isDmxActive) {
    sprintf(msBuf, "%2.1f ms", interval);
  } else {
    sprintf(msBuf, "--- ms");
  }

  // DMX Load (アクティブチャンネル率)
  int activeCh = 0;
  for (int c = 0; c < 512; c++) {
    if (dmxData[c] > 0) activeCh++;
  }
  int load = (activeCh * 100) / 512;
  char loadBuf[16];
  sprintf(loadBuf, "%d%%", load);

  // 3. 1列のプレミアム縦リスト描画 (Y=19 から 12px 刻み)
  // 行0 (Y = 19): MODE
  canvas.setCursor(6, 19);
  canvas.setTextColor(0x05FF); // アイスブルー (高輝度)
  canvas.print("MODE:");
  canvas.setCursor(66, 19);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.print(getCommModeNameShort(currentCommMode));

  // 行1 (Y = 31): UPTIME
  canvas.setCursor(6, 31);
  canvas.setTextColor(0x05FF);
  canvas.print("UPTIME:");
  canvas.setCursor(66, 31);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.print(uptimeBuf);

  // 行2 (Y = 43): SD CARD
  canvas.setCursor(6, 43);
  canvas.setTextColor(0x05FF);
  canvas.print("SD CARD:");
  canvas.setCursor(66, 43);
  if (isSdInitialized) {
    canvas.setTextColor(0x07E0); // クリーンなグリーン (高輝度)
    canvas.print("READY");
  } else {
    canvas.setTextColor(0xFD20); // 警告オレンジ (高輝度)
    canvas.print("NO CARD");
  }

  // 行3 (Y = 55): FILE
  canvas.setCursor(6, 55);
  canvas.setTextColor(0x05FF);
  canvas.print("FILE:");
  canvas.setCursor(66, 55);
  if (isFileOpened || isPlaybackFileOpened) {
    canvas.setTextColor(0x05FF); // アイスブルー
    canvas.print(selectedFileName);
  } else {
    canvas.setTextColor(ST77XX_WHITE); // 白 (高輝度)
    canvas.print("STANDBY");
  }

  // 行4 (Y = 67): DMX RATE (FPS)
  canvas.setCursor(6, 67);
  canvas.setTextColor(0x05FF);
  canvas.print("DMX RATE:");
  canvas.setCursor(66, 67);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.print(fpsBuf);

  // 行5 (Y = 79): INTERVAL
  canvas.setCursor(6, 79);
  canvas.setTextColor(0x05FF);
  canvas.print("INTERVAL:");
  canvas.setCursor(66, 79);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.print(msBuf);

  // 行6 (Y = 91): DMX LOAD
  canvas.setCursor(6, 91);
  canvas.setTextColor(0x05FF);
  canvas.print("DMX LOAD:");
  canvas.setCursor(66, 91);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.print(loadBuf);

  // 行7 (Y = 103): LOAD プログレスバー (プレミアム仕様)
  canvas.drawRect(6, 103, 148, 6, 0x2104); // 暗いスレート枠線
  int barW = (load * 144) / 100;
  if (barW > 0) {
    canvas.fillRect(8, 104, barW, 4, 0x05FF); // アイスブルーの塗りつぶし
  }

  // 4. フッター描画 (高輝度白文字で視認性向上)
  canvas.fillRect(0, 114, 160, 14, 0x2104);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setCursor(6, 117);
  canvas.print("[BACK] RETURN TO SELECT");

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

// ★大幅リニューアル: ダブルバッファリング版 Fixture Inspector (チラつき完全ゼロ ＆ 10灯切り替え ＆ 2ページ対応)
void drawMonitorFixture() {
  canvas.fillScreen(ST77XX_BLACK);
  
  LIGHT_SET &light = lightSets[selectedLightSetIdx];
  int startDmxIndex = light.farstCh - 1; // 0-indexed DMX start channel
  int totalPages = (light.ChLength > 8) ? 2 : 1;
  
  // 1. ヘッダー描画
  canvas.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレーのヘッダーバー
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  canvas.setCursor(6, 4);
  
  char headerBuf[32];
  if (totalPages > 1) {
    sprintf(headerBuf, "FIX %d/10 P.%d: %s", selectedLightSetIdx + 1, fixturePage + 1, light.LightName);
  } else {
    sprintf(headerBuf, "FIX %d/10: %s", selectedLightSetIdx + 1, light.LightName);
  }
  canvas.print(headerBuf);
  canvas.drawFastHLine(0, 15, 160, 0x528A); // セパレーター線

  // 2. パラメータリストとメーターの描画 (8ch分)
  int chOffset = fixturePage * 8;
  int drawChCount = min(light.ChLength - chOffset, 8);
  int ySpacing = 11;

  for (int i = 0; i < drawChCount; i++) {
    int localChIdx = chOffset + i;
    int chIndex = startDmxIndex + localChIdx;
    int y = 18 + (i * ySpacing);

    if (chIndex < 512) {
      uint8_t val = dmxData[chIndex];

      // CHアドレスと機能名
      canvas.setTextColor(ST77XX_WHITE);
      canvas.setCursor(4, y);
      char chLabel[24];
      sprintf(chLabel, "CH%03d %s:", chIndex + 1, getChModeName(light.ch[localChIdx]));
      canvas.print(chLabel);

      // メーターの描画 (X=90 から幅 42px)
      int meterWidth = (val * 42) / 255;
      if (meterWidth > 0) {
        canvas.fillRect(90, y, meterWidth, 7, 0x05FF);
      }
      canvas.drawRect(89, y - 1, 44, 9, 0x2104);

      // 数値の描画 (X=138 から3桁)
      canvas.setCursor(138, y);
      canvas.setTextColor(val > 0 ? ST77XX_WHITE : 0x528A);
      char valBuf[8];
      sprintf(valBuf, "%3d", val);
      canvas.print(valBuf);
    }
  }

  // 3. フッター描画 (操作ガイド)
  canvas.fillRect(0, 114, 160, 14, 0x2104);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setCursor(6, 117);
  
  if (totalPages > 1) {
    canvas.print("[LF/RT] PAGE  [UP/DN] FIX");
  } else {
    canvas.print("[UP/DN] CHANGE FIXTURE");
  }

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

// ★追加: ボタン長押し時の高速オートリピート処理
void handleAutoRepeat() {
  static uint32_t lastRepeatTime = 0;
  
  if (sysState == RUNNING && currentCommMode == COMM_MANUAL_WRITE) {
    // 1. LEFTボタン長押し (値下げる)
    if (swt[3].isPressed && (millis() - swt[3].pressStartTime > 300)) {
      uint32_t pressDuration = millis() - swt[3].pressStartTime;
      uint32_t interval = (pressDuration > 1000) ? 5 : 15; // 1秒超で超高速化 (5ms), それ以外は15ms
      if (millis() - lastRepeatTime > interval) {
        if (dmxData[manualSelectedCh] > 0) {
          dmxData[manualSelectedCh]--;
          requestDraw = true;
        }
        lastRepeatTime = millis();
      }
    }
    // 2. RIGHTボタン長押し (値上げる)
    else if (swt[4].isPressed && (millis() - swt[4].pressStartTime > 300)) {
      uint32_t pressDuration = millis() - swt[4].pressStartTime;
      uint32_t interval = (pressDuration > 1000) ? 5 : 15; // 1秒超で超高速化 (5ms), それ以外は15ms
      if (millis() - lastRepeatTime > interval) {
        if (dmxData[manualSelectedCh] < 255) {
          dmxData[manualSelectedCh]++;
          requestDraw = true;
        }
        lastRepeatTime = millis();
      }
    }
    // 3. UPボタン長押し (チャンネル下げる)
    else if (swt[1].isPressed && (millis() - swt[1].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[1].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 30 : 80; // 3秒超で超高速化 (30ms)
      if (millis() - lastRepeatTime > interval) {
        if (manualSelectedCh > 0) {
          manualSelectedCh--;
        } else {
          manualSelectedCh = 511;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
    // 4. DOWNボタン長押し (チャンネル上げる)
    else if (swt[2].isPressed && (millis() - swt[2].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[2].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 30 : 80; // 3秒超で超高速化 (30ms)
      if (millis() - lastRepeatTime > interval) {
        if (manualSelectedCh < 511) {
          manualSelectedCh++;
        } else {
          manualSelectedCh = 0;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
  }
  else if (sysState == RUNNING && currentDispMode == DISP_MULTI_LIGHT) {
    // 1. UPボタン長押し (スクロール上)
    if (swt[1].isPressed && (millis() - swt[1].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[1].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 50 : 150;
      if (millis() - lastRepeatTime > interval) {
        Cursor.Up();
        lastRepeatTime = millis();
      }
    }
    // 2. DOWNボタン長押し (スクロール下)
    else if (swt[2].isPressed && (millis() - swt[2].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[2].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 50 : 150;
      if (millis() - lastRepeatTime > interval) {
        Cursor.Down();
        lastRepeatTime = millis();
      }
    }
    // 3. LEFTボタン長押し (アドレス下げる)
    else if (swt[3].isPressed && (millis() - swt[3].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[3].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 20 : 60;
      if (millis() - lastRepeatTime > interval) {
        LIGHT_SET &l = lightSets[selectedLightSetIdx];
        if (l.farstCh > 1) {
          l.farstCh--;
        } else {
          l.farstCh = 513 - l.ChLength;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
    // 4. RIGHTボタン長押し (アドレス上げる)
    else if (swt[4].isPressed && (millis() - swt[4].pressStartTime > 500)) {
      uint32_t pressDuration = millis() - swt[4].pressStartTime;
      uint32_t interval = (pressDuration > 3000) ? 20 : 60;
      if (millis() - lastRepeatTime > interval) {
        LIGHT_SET &l = lightSets[selectedLightSetIdx];
        if (l.farstCh < 513 - l.ChLength) {
          l.farstCh++;
        } else {
          l.farstCh = 1;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
  }
  else if (sysState == EDITING_FIXTURE) {
    // 1. UPボタン長押し (項目上)
    if (swt[1].isPressed && (millis() - swt[1].pressStartTime > 500)) {
      if (millis() - lastRepeatTime > 150) {
        Cursor.Up();
        lastRepeatTime = millis();
      }
    }
    // 2. DOWNボタン長押し (項目下)
    else if (swt[2].isPressed && (millis() - swt[2].pressStartTime > 500)) {
      if (millis() - lastRepeatTime > 150) {
        Cursor.Down();
        lastRepeatTime = millis();
      }
    }
    // 3. LEFTボタン長押し (役割/長さを高速変更)
    else if (swt[3].isPressed && (millis() - swt[3].pressStartTime > 500)) {
      if (millis() - lastRepeatTime > 100) {
        LIGHT_SET &l = lightSets[selectedLightSetIdx];
        if (editChCursor < l.ChLength) {
          int currentMode = (int)l.ch[editChCursor];
          if (currentMode > 0) currentMode--;
          else currentMode = 11;
          l.ch[editChCursor] = (ChMode)currentMode;
        } else if (editChCursor == l.ChLength) {
          if (l.ChLength > 1) l.ChLength--;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
    // 4. RIGHTボタン長押し (役割/長さを高速変更)
    else if (swt[4].isPressed && (millis() - swt[4].pressStartTime > 500)) {
      if (millis() - lastRepeatTime > 100) {
        LIGHT_SET &l = lightSets[selectedLightSetIdx];
        if (editChCursor < l.ChLength) {
          int currentMode = (int)l.ch[editChCursor];
          if (currentMode < 11) currentMode++;
          else currentMode = 0;
          l.ch[editChCursor] = (ChMode)currentMode;
        } else if (editChCursor == l.ChLength) {
          if (l.ChLength < 16) l.ChLength++;
        }
        lastRepeatTime = millis();
        requestDraw = true;
      }
    }
  }
}
// ★追加: マニュアルDMX出力処理 (Core 0)
void processManualWrite() {
  digitalWrite(MAX485_RE_DE, HIGH); // DMX送信モード
  dmxOut.write((uint8_t*)dmxData, 512);
  delay(20); // DMX出力周期の安定化 (約50fps)
}

// ★追加: マニュアルコントローラー専用の極美麗UI描画関数 (Core 1) - ダブルバッファリング版（9行テーブル仕様）
void drawManualWrite() {
  // 1. キャンバス全体を黒でクリア
  canvas.fillScreen(ST77XX_BLACK);
  
  // 2. 静的ヘッダーの描画
  canvas.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレー
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  canvas.setCursor(6, 4);
  canvas.print("MANUAL DMX CONTROLLER");
  canvas.drawFastHLine(0, 15, 160, 0x528A); // セパレーター線
  
  // 3. 9行周辺チャンネルリストの描画 (i = -4 から 4 まで)
  // アクティブチャンネル of 背景ハイライト (Y=59, 高さ9)
  canvas.fillRoundRect(4, 59, 152, 9, 1, 0x05FF);
  
  int ySpacing = 10;
  int startY = 20; // Y=20 から10px刻みで描画
  
  for (int i = -4; i <= 4; i++) {
    int ch = manualSelectedCh + i;
    int y = startY + (i + 4) * ySpacing;
    
    if (ch >= 0 && ch < 512) {
      uint8_t val = dmxData[ch];
      
      if (i == 0) {
        // 現在選択中のチャンネル (アイスブルー背景の上に黒文字でくっきり表示)
        canvas.setTextColor(ST77XX_BLACK);
        canvas.setCursor(8, y);
        char rowBuf[32];
        sprintf(rowBuf, "> CH %03d: %3d", ch + 1, val);
        canvas.print(rowBuf);
        
        // アクティブ行（Y=60）の右側にインラインミニバー（スライダー）を描画
        // アイスブルーの背景の上なので、枠線とバーの中身を黒(ST77XX_BLACK)で描画します
        int barW = (val * 44) / 255;
        canvas.drawRect(100, y + 1, 46, 6, ST77XX_BLACK); // 黒い枠線
        if (barW > 0) {
          canvas.fillRect(101, y + 2, barW, 4, ST77XX_BLACK); // 黒いインラインバー
        }
      } else {
        // 周辺チャンネル (白文字に変更して視認性を大幅に向上！)
        canvas.setTextColor(ST77XX_WHITE);
        canvas.setCursor(8, y);
        char rowBuf[32];
        sprintf(rowBuf, "  CH %03d: %3d", ch + 1, val);
        canvas.print(rowBuf);
        
        // 非アクティブ行の右側にもインラインミニバーを描画して視覚的に調光状態を一覧可能に
        // 暗い背景（黒）の上なので、枠線はダークグレー、中身はグレーで描画します
        int barW = (val * 44) / 255;
        canvas.drawRect(100, y + 1, 46, 6, 0x2104); // ダークグレーの枠線
        if (barW > 0) {
          canvas.fillRect(101, y + 2, barW, 4, 0x528A); // グレーのインラインバー
        }
      }
    }
  }
  
  // 4. フッターの描画
  canvas.fillRect(0, 114, 160, 14, 0x2104); // フッターバー
  canvas.setTextColor(ST77XX_WHITE); // 白文字にして明るさを改善！
  canvas.setCursor(6, 117);
  canvas.print("[UP/DN] CH   [LF/RT] VAL");
  
  // 5. メモリ上のキャンバスを一気にTFT画面へ物理転送 (チラつき完全ゼロ)
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

// ★追加: 10灯マルチライトモニター表示関数 (Core 1)
void drawMonitorMultiLight() {
  canvas.fillScreen(ST77XX_BLACK);
  
  // 1. スクロールレールの描画 (右端)
  canvas.drawFastVLine(155, 0, 128, 0x2104);
  int barH = 51; // 128 * 4 / 10
  int barY = scrollOffset * (128 - barH) / 6;
  canvas.fillRect(154, barY, 4, barH, 0x05FF); // アイスブルーのスクロールバー
  
  // 2. 4つのライトプレビューカードを描画
  for (int i = 0; i < 4; i++) {
    int lightIdx = scrollOffset + i;
    if (lightIdx >= 10) break;
    
    LIGHT_SET &l = lightSets[lightIdx];
    int y = i * 32;
    bool isSelected = (lightIdx == selectedLightSetIdx);
    
    // カード背景と枠線の描画
    if (isSelected) {
      canvas.fillRect(0, y, 152, 31, 0x10A2); // やや明るいダークグレー背景
      canvas.drawRect(0, y, 152, 31, 0x05FF); // アイスブルー枠
    } else {
      canvas.drawRect(0, y, 152, 31, 0x2104); // ダークグレー枠
    }
    
    // ライトの基本情報
    // タイプ名
    const char* typeStr = "USER";
    if (l.type == TYPE_PAR) typeStr = "PAR";
    else if (l.type == TYPE_LINEAR) typeStr = "LIN";
    
    // テキスト色（ユーザー要望に基づき明るい色に）
    uint16_t txtColor = isSelected ? ST77XX_WHITE : 0x7BEF;
    canvas.setTextColor(txtColor);
    
    // インデックスとタイプ名
    canvas.setCursor(6, y + 4);
    char nameBuf[24];
    sprintf(nameBuf, "%2d:%s", lightIdx + 1, typeStr);
    canvas.print(nameBuf);
    
    // DMXアドレス範囲
    canvas.setCursor(6, y + 19);
    char addrBuf[24];
    sprintf(addrBuf, "A:%03d-%03d", l.farstCh, l.farstCh + l.ChLength - 1);
    canvas.print(addrBuf);
    
    // DMXデータから各役割の値を取得
    uint8_t rVal = 0, gVal = 0, bVal = 0, wVal = 0, dimVal = 255, strobeVal = 0;
    uint8_t panVal = 0, tiltVal = 0;
    bool hasDim = false, hasStrobe = false, hasPan = false, hasTilt = false;
    for (int chIdx = 0; chIdx < l.ChLength; chIdx++) {
      int dmxCh = l.farstCh - 1 + chIdx;
      if (dmxCh < 512) {
        uint8_t val = dmxData[dmxCh];
        switch (l.ch[chIdx]) {
          case DIMMER: dimVal = val; hasDim = true; break;
          case RED: rVal = val; break;
          case GREEN: gVal = val; break;
          case BLUE: bVal = val; break;
          case WHILE: wVal = val; break;
          case STROBE: strobeVal = val; hasStrobe = true; break;
          case MOVE_X: panVal = val; hasPan = true; break;
          case MOVE_Y: tiltVal = val; hasTilt = true; break;
          default: break;
        }
      }
    }
    
    // Dimmer値の表示
    canvas.setCursor(68, y + 4);
    if (hasDim) {
      char dimBuf[16];
      sprintf(dimBuf, "D:%3d", dimVal);
      canvas.print(dimBuf);
    } else {
      canvas.print("D:---");
    }
    
    // Strobe値の表示
    canvas.setCursor(68, y + 19);
    if (hasStrobe) {
      char strbBuf[16];
      sprintf(strbBuf, "S:%3d", strobeVal);
      canvas.print(strbBuf);
    } else {
      canvas.print("S:---");
    }
    
    // リアルタイムRGBW混色カラープレビュー枠
    uint16_t finalR = ((uint32_t)rVal * dimVal) / 255;
    uint16_t finalG = ((uint32_t)gVal * dimVal) / 255;
    uint16_t finalB = ((uint32_t)bVal * dimVal) / 255;
    uint16_t addW = ((uint32_t)wVal * dimVal) / 255;
    finalR = min(255, finalR + addW);
    finalG = min(255, finalG + addW);
    finalB = min(255, finalB + addW);
    
    uint16_t previewColor = tft.color565(finalR, finalG, finalB);
    
    // カラープレビュー枠描画
    canvas.fillRect(118, y + 7, 18, 17, previewColor);
    canvas.drawRect(117, y + 6, 20, 19, 0x528A); // スレートグレーのプレビュー枠線
    
    // 背景色（クリア用）
    uint16_t cardBgColor = isSelected ? 0x10A2 : ST77XX_BLACK;
    
    // Pan バー (左横)
    if (hasPan) {
      canvas.drawRect(112, y + 6, 4, 19, 0x528A); // 枠線
      canvas.fillRect(113, y + 7, 2, 17, cardBgColor); // 背景クリア
      int panH = ((int)panVal * 17) / 255;
      if (panH > 0) {
        canvas.fillRect(113, y + 7 + 17 - panH, 2, panH, 0x05FF); // アイスブルーの縦バー
      }
    }
    
    // Tilt バー (下側)
    if (hasTilt) {
      canvas.drawRect(117, y + 26, 20, 4, 0x528A); // 枠線
      canvas.fillRect(118, y + 27, 18, 2, cardBgColor); // 背景クリア
      int tiltW = ((int)tiltVal * 18) / 255;
      if (tiltW > 0) {
        canvas.fillRect(118, y + 27, tiltW, 2, 0x05FF); // アイスブルーの横バー
      }
    }
  }
  
  // 物理画面へ転送
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

// ★追加: Fixture エディターUI描画関数 (Core 1)
void drawFixtureEditor() {
  canvas.fillScreen(ST77XX_BLACK);
  
  LIGHT_SET &l = lightSets[selectedLightSetIdx];
  
  // 1. ヘッダーの描画
  canvas.fillRect(0, 0, 160, 15, 0x2104); // ダークスレートグレー
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  canvas.setCursor(6, 4);
  char headBuf[32];
  sprintf(headBuf, "EDIT LGT %d: %s", selectedLightSetIdx + 1, l.LightName);
  canvas.print(headBuf);
  canvas.drawFastHLine(0, 15, 160, 0x528A); // セパレーター
  
  // 2. スクロールオフセットの計算 (表示最大8行)
  int editScrollOffset = 0;
  if (editChCursor >= 8) {
    editScrollOffset = editChCursor - 7;
  }
  
  // 3. リストの描画
  int ySpacing = 11;
  int startY = 18;
  
  for (int i = 0; i < 8; i++) {
    int idx = editScrollOffset + i;
    if (idx > l.ChLength) break; // 総チャンネル長の行まで
    
    int y = startY + i * ySpacing;
    bool isCursorRow = (idx == editChCursor);
    
    if (isCursorRow) {
      canvas.fillRoundRect(4, y - 1, 152, 10, 1, 0x05FF); // アイスブルーの背景
      canvas.setTextColor(ST77XX_BLACK);
    } else {
      canvas.setTextColor(ST77XX_WHITE);
    }
    
    if (idx < l.ChLength) {
      // チャンネル役割変更
      canvas.setCursor(8, y);
      if (isCursorRow) {
        char chBuf[32];
        sprintf(chBuf, " CH %02d: < %s >", idx + 1, getChModeName(l.ch[idx]));
        canvas.print(chBuf);
      } else {
        char chBuf[32];
        sprintf(chBuf, "  CH %02d:   %s", idx + 1, getChModeName(l.ch[idx]));
        canvas.print(chBuf);
      }
    } else {
      // 総チャンネル長
      canvas.setCursor(8, y);
      if (isCursorRow) {
        char lenBuf[32];
        sprintf(lenBuf, " LENGTH: < %2d CH >", l.ChLength);
        canvas.print(lenBuf);
      } else {
        char lenBuf[32];
        sprintf(lenBuf, "  LENGTH:   %2d CH", l.ChLength);
        canvas.print(lenBuf);
      }
    }
  }
  
  // 4. フッターの描画
  canvas.drawFastHLine(0, 113, 160, 0x528A);
  canvas.fillRect(0, 114, 160, 14, 0x2104);
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setCursor(6, 117);
  canvas.print("[UP/DN] SEL  [LF/RT] CHG");
  
  // 物理画面へ転送
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}