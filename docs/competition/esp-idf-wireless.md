# ESP-IDF／FreeRTOS、OTA 與無線除錯

## 交付範圍與計劃

本次延續比賽程式，改用 ESP32-S3 N16R8、native ESP-IDF、PlatformIO，
保留原 pinout、三顆 ToF 和四角 QRE。使用者確認雙 OTA 分區、失敗回復
與保留 NVS，要求 UDP 及 ESP-NOW 兩種遙測、動作計時和桌面 bench 駕駛。
接收端亦採 S3，本交付使用相同 N16R8 environment。

實作依次處理 native 感測器移植、FreeRTOS 所有權、無線／OTA、桌面工具、
host fault tests 和完整 firmware gate。現有 Arduino 程式保留作參考；
共用策略新增短動作間的觀察，因此仍需編譯全部舊 sketch。沒有遠端
publish、flash、啟動馬達或實機測試授權；提供程式與操作方法。

## Build 與硬件

- PlatformIO Core **6.2.0**、`platformio/espressif32@7.0.1`、ESP-IDF **6.0.1**。
  版本對應由 [PlatformIO 官方 release](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.1) 核對。
- 根目錄 [platformio.ini](../../platformio.ini)：`robot` 及 `espnow_receiver`。
- [N16R8 pinout](../hardware/esp32-s3-devkitc-1-n16r8-pinout.md)：quad flash、
  octal PSRAM，不能沿用 N32R16／N16R8V 的 octal-flash 設定。
- Firmware 使用 native GPIO、ADC1、LEDC、I2C master、NVS 和 PSA crypto，
  **不依賴 Arduino runtime**。VL53L0X 元件保留 Pololu/ST license。

```bash
platformio run -e robot -e espnow_receiver
platformio run -e robot -t upload
platformio device monitor
```

`upload` 由使用者在硬件確認後執行。第一回須 USB 寫入 bootloader、分區
和映像，之後才能 OTA。需要原生 USB 19/20 對應的 USB Serial/JTAG 插座。
若開發板同時有 USB-UART 插座，兩者不要混淆。

## 排程與實際動作時間

| 工作 | Core／priority | 目標時間與邊界 |
| --- | --- | --- |
| control | 1／20 | 每 5ms；ADC、避邊、按鈕、PWM；不讀 I2C／socket／NVS |
| ToF | 0／6 | `C–L–C–R` 依次曝光，每次 budget 33ms；單次 timeout 80ms |
| wireless | 0／3 | 每 20ms，50Hz；84 bytes 最新快照；不排隊重送舊快照 |
| HTTP OTA | 0／3 | 寫 flash 前等待 control 確認 PWM=0，禁止行車 |
| service | 0／2 | USB命令和停止狀態下的 NVS保存；控制不等待 USB輸出 |

雙核／240MHz、1kHz FreeRTOS tick、80MHz PSRAM 都已指定。控制資料和
task stacks 使用內部 RAM；PSRAM 可供普通配置／緩衝使用，不把控制
資料移入較慢的外部記憶體。保留 CPU 餘量讓 Wi-Fi、idle 和 watchdog 工作。
不是把 CPU 無目的地跑到 100%。

| 動作 | 時間／條件 |
| --- | --- |
| 開賽去抖／等待 | 30ms／1000ms；局計時由啟動開始 |
| 搜尋一段 | 轉 320ms → 停看 180ms → 前進 300ms → 停看 200ms |
| 搜尋方向 | 每段交替；不是已知航向或固定地圖 |
| 側面目標置中／中央接近 | 由新鮮距離資料決定，不硬寫前進秒數 |
| 推進 | 中央兩次確認、35–150mm；PWM175；修正 ±25 |
| 長時間推進脫困 | 6000ms 推進後，退 150ms，再轉 250ms |
| 前／後避邊 | 持續檢查另一端；全角安全持續40ms才結束；300ms為失敗上限 |
| 推進失去中央目標／避邊後 | PWM=0，HOLD3500ms；不宣稱勝出 |
| 電氣換向 | 先PWM=0，至少25ms才改DIR；增加PWM每tick最多12 |
| bench 心跳 | 桌面40ms；browser租期100ms；robot租期150ms，過期停機 |
| OTA窗口 | 停止時按住BUTTON2兩秒，開啟60秒；一次上傳消耗窗口 |
| 局限時 | 120000ms |

中心優先理論上約每70ms更新中心、140ms更新兩側，仍要量測實際時間。
`control_max_us` 記錄最長執行；`deadline_misses` 包含執行大於5ms或
開始間隔大於7ms。這些是診斷值，不是硬即時承諾。
桌面用裝置 `uptime_ms/state_elapsed_ms` 顯示狀態與持續時間。
50Hz快照可能漏掉極短 transition；不是無損 tracing recorder。

## 兩種無線路徑

1. **UDP**：機械人開 WPA2 AP `BottleSumo`，電腦連線後啟動工具。
   電腦每秒 `SUB` 登記單播目的地；快照和 bench命令使用UDP4242。
2. **ESP-NOW**：第二塊S3作接收橋，以同一channel、PMK、LMK加密單播，
   經USB傳 `T:` 快照至電腦；`C:` bench命令反向轉發至機械人。

同時支援兩路。兩路共用序號和boot ID，重複／舊資料可剔除。ESP-NOW
等前一個send callback後才送，回調只複製小封包或更新計數。
單筆queue保存最新資料，鏈路壅塞不阻塞控制 task。禁用 Wi-Fi modem sleep，
選擇現場較空的固定channel，避免掃描和斷線重連延遲。
這些措施依據 [Espressif ESP-NOW 文件](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/network/esp_now.html)。

封包不足250 bytes。資料缺口、MAC送出成功／失敗和UDP本地送出失敗
都有計數。MAC成功不保證應用收件；序號缺口能顯示丟失或橋接queue淘汰。
電腦的age是最後收件至今的時間，不是未同步時鐘下的單向延遲。
沒有實機RF測試，不能保證指定延遲或丟包率。

## macOS／Windows工具

安裝 `uv`，使用 [desktop debugger](../../tools/bottlesumo_debug/README.md)。
UDP／圖表／OTA只用Python標準庫；USB配置和serial模式由 `uv` 執行時固定帶入
`pyserial==3.5`。

```bash
uv run --frozen tools/bottlesumo_debug/debugger.py configure
uv run --frozen tools/bottlesumo_debug/debugger.py provision --serial PORT
uv run --frozen tools/bottlesumo_debug/debugger.py gui
uv run --frozen tools/bottlesumo_debug/debugger.py gui --serial PORT
```

macOS和Windows使用同一組 `uv` 命令。雙擊
[run-macos.command](../../tools/bottlesumo_debug/run-macos.command) 或
[run-windows.cmd](../../tools/bottlesumo_debug/run-windows.cmd) 即開除錯頁面。
macOS需檔案有執行權限。USB模式PORT例如Windows COM埠或macOS裝置埠。

先build、USB燒錄兩個environment，從開機訊息取得兩塊STA MAC。
`configure --robot-mac MAC --receiver-mac MAC` 只建立／更新 ignored 的
`wireless.local.json`；不在console印密碼或token，不產生含秘密的header。
對兩塊板各執行 `provision --serial PORT`，收到確認後手動reset。
配置保存在NVS，OTA映像不含AP密碼、router密碼或更新金鑰。
從本機JSON查看Wi-Fi密碼，不貼到chat或commit。未配置時無線關閉。
reconfigure保留已有keys；provision是一次明確寫入，不盲重試模糊結果。

在 [network_config.h](../../firmware/bottlesumo_idf/main/network_config.h) 選擇
`TransportMode::AP_UDP`、`TransportMode::ESP_NOW` 或 `TransportMode::BOTH`。
預設BOTH。AP在三種模式均保留，用作OTA和維護；ESP_NOW模式拒絕UDP駕駛。
ESP_NOW需要非零配對MAC；AP_UDP不啟動ESP-NOW。

AP＋STA：在私密JSON填 `sta_ssid` 和 `sta_password`，再provision及reset。
不要把密碼放在CLI參數。SSID留空表示只開AP；非空使用WPA2 router。
STA連上router時AP也跟隨routerchannel；ESP-NOW peer使用目前channel。
接收板連到robot AP以跟隨頻道，AP可同時容納電腦和receiver兩個client。
STA只在停止時每五秒嘗試連線／重連，自主比賽期間不掃描重連。
Router改channel仍會造成radio中斷，不能藉雙核消除同一radio的限制。
電腦可直接連AP，或同router下以 `gui --robot ROBOT_STA_IP` 接收。
STA IP可由router的DHCP名單與robot MAC核對。

## 駕駛、校準與更新互鎖

- `HARDWARE_CONFIRMED=false` 預設禁動。按前述commissioning指南核對輪向、
  QRE及煞停距離，改native `robot.cpp` 內的確認值後重編。
- 上電時按住BUTTON2選bench；放開後方可操作。網絡不能改模式。
  自主模式照常BUTTON1開賽，所有網絡駕駛命令被拒絕。
- USB `f/e` 三秒floor／void校準，`q` 看狀態，`x` 停止。四角順序及極性
  不變。NVS寫入完成前禁動；不自動erase NVS。
- 桌面調速度／轉向或左右PWM，按住按钮／WASD才駕駛，Space停止。
  放手、失焦、隱藏頁面或連線過期後不再續租。
  bench PWM預設上限±120，避邊、停止按鈕、fault和OTA永遠優先。
- 命令有 HMAC-SHA256、boot nonce及單調序號，過期／重播不能續租。
  localhost控制端也驗證Origin、Host、CSRF及request順序。
- OTA只接受已停止且實體開窗口的更新；image長度、SHA256和HMAC先核對。
  TCP傳輸完成及ESP-IDF驗證後才切換boot slot，不盲重試模糊結果。
  USB配置與OTA以原子取得維護控制權，不能同時寫入或互相解除停止鎖。

```bash
uv run --frozen tools/bottlesumo_debug/debugger.py ota --firmware .pio/build/robot/firmware.bin
```

OTA使用私有WPA2 AP上的HTTP加HMAC映像驗證，不是互聯網HTTPS更新服務。
ESP-NOW使用配對加密。沒有burn eFuse、Secure Boot或flash encryption。
NVS位於0x9000；兩個等大OTA slot保留其餘flash。遷移首次USBflash必須
更新bootloader及partition table，不能把Arduino映像直接當作IDF OTA。
第一次遷移前應重新記錄QRE校準，分區更換不等於資料遷移保證。

新OTA映像在5秒後檢查ADC／PWM初始化、ToF新鮮資料、8MB PSRAM和無線
task heartbeat才確認；失敗回復前一個有效slot。rollback依據
[Espressif OTA流程](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/system/ota.html)。
這不驗證射頻品質、實際輪向、接觸或制動，也不能救回任意損壞的bootloader。

## 策略來源與比賽效益

按使用者要求，已在Poe網站向官方 [GPT-6-Astra](https://poe.com/GPT-6-Astra)
送一次硬件／規則摘要，取得完整建議。沒有傳source、私人檔案或秘密。
採納中心優先、短轉動後觀察、記錄時序等可測試想法。Astra誤把既有
3500ms HOLD當作盲推；實際已是停車，這項誤讀未採用。
不加入未證實的FG RPM控制、相機識別或「已勝出」判定。

比較原搜尋與新策略時，做各十組配對試驗：相同起點、枱面、電量和目標
姿態；記錄捕獲時間、跌落、誤避邊、連續正面接觸、裁判判定移除及控制
超時。一次只改一個PWM或時間值；只有實測才可聲稱勝率改善。

## 本地交付驗證（2026-10-13）

完整 `scripts/validate-firmware.sh` 以隔離 Arduino-ESP32 3.3.8 成功編譯
三個 Arduino sketch，以及 PlatformIO 的 `robot`／`espnow_receiver`。
Native 使用 ESP-IDF 6.0.1；build 報告 robot 靜態 RAM 37596 bytes、
映像 853511 bytes，receiver 靜態 RAM 37008 bytes、映像 750027 bytes。
這些數字不含所有 runtime 配置，不能代替 stack／heap 實機量測。
舊 Arduino core／測試仍有既有 warnings；native 最終 build 沒有 compiler warning。

策略、Arduino adapter、wireless framing assertions、15項 Python測試、
production JS 的 STOP 回歸測試全部通過。隔離移除 key-repeat 修正時，
STOP 測試確實失敗。Native ToF 測試覆蓋160個初始化／budget故障點和
12個runtime故障點。兩環境產生的 sdkconfig、16MB分區邊界、CI契約、
local Markdown links、shell語法及 `git diff --check` 均已核對。

桌面頁面用標明模擬的資料檢查圖表和動作時間，未連接或駕駛robot。
獨立唯讀覆核已解決 STOP 重複按鍵、OTA 健康檢查及 USB／OTA 維護競態；
primary 最終架構評估核對了單一PWM所有權、維護停止確認、命令租期、
憑證存放和故障優先序，沒有剩餘重大 finding。

本次完成程式與host驗證。沒有 USB燒錄、實際配置寫入、射頻／router、
powered OTA／rollback、Windows啟動腳本或比賽實測；未commit／push／merge。
保留 `HARDWARE_CONFIRMED=false`，需commissioning後再啟用馬達。
