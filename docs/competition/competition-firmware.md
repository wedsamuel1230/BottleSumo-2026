# BottleSumo competition firmware

Active development has moved to [ESP-IDF/PlatformIO on N16R8](esp-idf-wireless.md).
This page preserves the earlier Arduino delivery. The shared search strategy
now uses 320ms turn / 180ms observe / 300ms advance / 200ms observe.

## Scope and implementation plan

The requested deliverable is autonomous senior-division firmware for the existing
v2 controller, plus a practical strategy and commissioning instructions. The
user confirmed QRE order: front-left, front-right, rear-left, rear-right on
GPIO1/2/4/5; motor 1 is left. Three VL53L0X heads use XSHUT GPIO6/7/15,
approved by the user, on the existing SDA17/SCL16 bus. Preserve the electrical
test and unrelated CAD changes. No upload, powered test, push, or merge is part
of this delivery.

The attached [2026 Rev.1 PDF](2026_rules_06.pdf) controls this implementation.
The older [Rev.0 transcription](table-sumo-rules-2026.md) remains a historical
reference. Research and strategy details below distinguish rule facts from
engineering choices; the PDF does not grant permission to contact the organizer
or submit forms.

Discovery used the existing sensor-design proposal and test sketch. A single
ToF head is cheaper but requires scanning for bearing. The chosen three-head
design supports centering with the confirmed wiring. Five heads or a camera
could improve classification, but would expand the hardware beyond this task.

Implementation follows these ordered slices in this chat:

1. `firmware/bottlesumo_competition/strategy.h` and
   `tests/competition-strategy-test.cpp`: test stop/timeout, every corner mask,
   stale ranges, directed acquisition, bounded recovery and post-push hold.
   Establish failing assertions against a stopped-only skeleton, then implement.
2. `firmware/bottlesumo_competition/bottlesumo_competition.ino`: pin-compatible
   adapter, address assignment, sequential ranging, calibration, guarded start,
   20 kHz PWM, reversal dead time, emergency stop and FG diagnostics.
3. This guide, the hardware reference, root README and documentation index:
   explain strategy, setup, limits and observed verification. Preserve existing
   references and unrelated changes.
4. Run the host assertions, all-sketch repository validator on CLI 1.4.1/core
   3.3.8, workflow contract and link checks, and a read-only independent review.

The primary blueprint assessment prioritizes edge/stop over all tactics, avoids
blocking range waits in the control loop, and prevents automatic motion without
calibration and a hardware-confirmation setting. It deliberately avoids RPM
control or odometry because the FG shaft reference is unverified. Acceptance
means the program and host checks pass; it never means an untested robot wins
or is safe on a powered table. Remaining physical gates must be named at handoff.

## 規則與勝出策略

以下規則以附上 PDF **2026 Rev.1** 的第 1、3–5 頁為依據。線上搜尋亦
檢查了[主辦方網站](https://ctea.org.hk/)，但未取得可核對的更新版賽規，
因此不能宣稱 Rev.1 是比賽當日最新版。現場公告和裁判裁決仍具優先權。

| 規則事實 | 本次實作／策略推論 |
| --- | --- |
| 標記正面必須持續接觸樽，直到樽跌出場外 | 先轉向置中，再正面推進；近距離只作小幅差速修正 |
| 推樽或成為最後一機後，仍須留在枱上至少 3 秒 | 推進時失去中央目標，或推進碰到枱邊後退回安全面，停留 3.5 秒 |
| 高級組枱面排列未知，膠紙顏色於檢查時公佈 | 不寫死路線；搜尋、前進和四角避邊循環，校準時包含膠紙 |
| 樽可能直立或橫躺 | 三個感測器放低，正面推板兼容兩種姿態 |
| 每局兩分鐘，30 秒無進展／無接觸等情況可判和 | 1 秒起步等待後主動搜尋；兩分鐘自動停機；6 秒推進後短暫脫困 |
| DIY 屬高級組；不超過 1 kg、3 馬達、2 控制器 | 本程式用一個 ESP32、兩個馬達；重量、尺寸、電池和離地高度另作實測 |
| 全自主，遙控只可啟動 | 比賽使用 BUTTON1 啟動，沒有無線或遙控轉向；Serial 校準只在停止時接受 |

建議的優先次序是：**留在枱上 → 找目標 → 置中 → 正面連續推 → 留場**。
單靠三個距離值仍不能可靠辨認水樽與對手，程式會推向可確認的前方目標，
以推樽或推對手出界兩種勝出方式作取捨。這是可測試的基線策略，並非勝率證明。
相同距離也不能證明前臉已接觸；失去目標也不能證明水樽已出界。

## 實際行為與調校值

| 狀態／事件 | 行為 |
| --- | --- |
| 上電／重設 | PWM=0；不自動開賽；不等待 USB 連線 |
| BUTTON1 新按下且條件全齊 | 去抖 30ms；等待 1 秒；局時計時由按下開始計算 |
| 搜尋 | 1000ms 原地掃描，800ms 低速前進；每段交替掃描方向 |
| 左／右頭看到目標 | 向相應方向轉，中央頭確認後才直行 |
| 中央目標 300–900mm | PWM145 接近；兩次獨立有效量測才確認目標 |
| 中央目標 151–299mm | PWM115 接近 |
| 中央目標 35–150mm | PWM175 推進；修正最多 ±25，兩輪均向前 |
| 推進時中央目標消失 | 立即停止並 HOLD 3500ms，然後重新搜尋 |
| 推進持續 6000ms | 後退 150ms、轉 250ms 再試；這是有時限的脫困，不是 FG 堵轉判定 |
| 只有前角檢出邊緣 | PWM105 後退；全角安全連續 40ms 後轉向或 HOLD |
| 只有後角檢出邊緣 | PWM105 前進；全角安全連續 40ms 後轉向或 HOLD |
| 前後角同時危險／逃離 300ms 仍未完成 | 停機；不猜測枱面形狀 |
| 一頭超過 80ms 未完成／I2C 錯誤或短讀 | 鎖定 fault、停機，修正後重設 |
| 任一頭的完整量測資料過期 250ms | 停止該局；需要重新按下 BUTTON1 |
| BUTTON2／Serial x／兩分鐘 | PWM立即歸零；重新開始需要放開並再按 BUTTON1 |

35–900mm、PWM 與所有時間值是保守的**起始調校值**。三頭依次單次曝光，
每頭預算 33ms；完整一輪通常約 100–130ms，兩次確認可需約 200–260ms。
控制目標每 5ms 檢查 QRE；I2C 操作、ADC 和系統排程會增加延遲。
Wire timeout 設 3ms，但不能把它視為整個系統的硬即時保證。
Pololu 初始化由 library 執行；運行時的單次啟動取自固定的 1.3.1 原始碼，
並檢查每次 I2C 寫入及讀回長度。沒有新 library 或 FG 閉環控制。

## 接線與感測器擺位

| 功能 | GPIO／地址 |
| --- | --- |
| QRE 左前／右前／左後／右後 | 1／2／4／5 |
| 左馬達 FG／PWM／DIR | 9／10／11 |
| 右馬達 FG／PWM／DIR | 12／13／14 |
| I2C SDA／SCL | 17／16 |
| ToF 左／中／右 XSHUT | 6／7／15 |
| ToF 左／中／右地址 | 0x30／0x31／0x32；每次上電重新分配 |
| 原 ToF interrupt | 18 保留為輸入；輪詢測距，不需並接三個 interrupt |
| LED／BUTTON1／BUTTON2 | 40／41／42；按鈕 HIGH 表示按下，保留外部下拉 |

三顆 ToF 共用 SDA/SCL；**拆除 XSHUT 固定接 3V3 的接法**，按確認的
GPIO6/7/15 分開控制。XSHUT 拉低時關閉，啟用時改為 INPUT，使用模組
自身 pull-up，避免向非 level-shifted XSHUT 強灌電壓。確認實際模組的
pull-up 和 3V3 I2C 電平；裸感測器與 breakout 不一定有相同電路。
GPIO18 可保留原有單頭 interrupt 接線，不用它作三頭識別。

建議中央頭朝正前方，左右頭分別向外約 20–25°，光窗距枱面約 65–75mm，
先以實際直立／橫躺水樽驗證。這些擺位是工程建議，不是本次量測結果。
讓推板從標記正面接觸樽，保持光路；光窗應位於推板接觸面後方至少約
40–50mm，使貼樽時仍能量到不小於 35mm 的距離。若樽被推板遮住，
程式會提早 HOLD，不能單靠改程式解決。

QRE 要在輪胎／必要支撐的危險支點之前看到落差。推板可比前 QRE 更突出，
讓樽先跌而車輪仍在枱上，但整體必須符合直徑、離地和非尖銳部件規定。
以最大實際速度測量反應距離與煞停距離，再決定感測器前伸量和 PWM。
反射式 QRE 不能保證識別所有落差；下方地面很近、黑膠紙、陽光或倒影
都可能令「枱面」與「空間」讀值重疊。重疊時應停止使用此調校，改善
感測器高度／遮光／位置，必要時換落差感測方案。

## 校準與開賽

1. 使用 PlatformIO 打開 [比賽 sketch 專案](../../firmware/bottlesumo_competition/platformio.ini)，
   保留同資料夾的 `strategy.h`。工具鏈與 ToF library 已固定在專案設定；
   全部程式可由 `./scripts/validate-firmware.sh` 編譯。
2. 初次保持 `HARDWARE_CONFIRMED=false`。核對接線、3V3 邏輯、PWM零時
   馬達是否真的停，以及兩輪正反向。分別調整 `FORWARD_LEVEL`。
   舊 electrical test 的 `MOTOR_TEST_DUTY=255` 實際為 100%，雖然文字說
   25%；不能照其文字當作低速測試。舊 `movement_test` 仍有 pin mismatch。
3. Motors 禁動時，Serial Monitor 設 115200。放在實際枱面，送 `f`。
   三秒內讓每個 QRE 都取樣安全白／灰表面與現場膠紙，不能包含真枱邊。
4. 用支架支撐車體，保持感測器正常高度，讓四個 QRE 同時看到真實落差，
   送 `e`。不是把車放高：這會改變下方反射背景。必要時以安全測試治具
   留空四個感測窗口。全程 PWM=0，不在比賽進行時操作。
5. 每角 floor／void 區間需至少相差 300 ADC counts；程式自動判斷極性，
   使用中點和 40-count hysteresis。失敗會拒絕啟動。校準存入 ESP32 NVS，
   重設可還原；換枱、光線、位置後重新做 `f` 和 `e`。
6. 送 `q`，逐角驗證 `edges` bit0/1/2/3 分別對應左前／右前／左後／右後。
   正常地面應是 `0x0`；接近各枱邊應準確觸發。驗證三個 ToF 的左右次序、
   中央鎖定、直立／橫躺樽、對手和近距離光路。
7. 用防墜治具和低速開始實機測試，量 PWM頻率、DIR換向、實際制動距離、
   BUTTON2停止，以及斷感測器後停止。這些由使用者在安全環境執行；
   本次沒有 upload 或開動實機。
8. 上述測試合格後，改 `HARDWARE_CONFIRMED=true`，重新編譯與燒錄。
   按裁判指示放置，按 BUTTON1 啟動並退到 1m 外；局內不接觸或發指令。

LED 熄滅表示未校準，慢閃表示已有校準且停止，長亮表示局內，快閃表示
fault；仍以 `q` 的 `confirmed/calibrated/fault` 作診斷。完成兩分鐘或
停止後，必須放開再按 BUTTON1 才能重新開始。I2C fault 必須修正後重設。
FG 預設不掛中斷，避免浮接噪聲；確定電氣安全後可改 `FG_CONNECTED=true`
看累積脈衝。脈衝不參與 RPM、里程或堵轉判定。

## 驗證記錄與重現

Host 測試直接使用 production 策略，並用標示為 host-only 的 Arduino
stubs 測試 adapter。Stubs 可檢查邏輯與故障停機，不能證明硬件行為。

```bash
c++ -std=c++17 -Wall -Wextra -Werror tests/competition-strategy-test.cpp -o /tmp/bottlesumo-strategy-test
/tmp/bottlesumo-strategy-test
c++ -std=c++17 -Wall -Wextra -Werror -Itests/arduino-stubs tests/competition-adapter-test.cpp -o /tmp/bottlesumo-adapter-test
/tmp/bottlesumo-adapter-test
./scripts/validate-firmware.sh
git diff --check
```

初始 stopped-only 策略在「中央目標應前進」assertion 失敗，實作後通過。
檢查涵蓋全部角落 mask、3500ms HOLD、持續越界停機、6000ms脫困、
250ms資料過期、120000ms停機、millis rollover、不同QRE極性與校準拒絕。
Adapter 檢查涵蓋校準取樣時間、NVS寫入、預設禁動、25ms換向等待、
PWM遞增、依次單次測距、I2C短讀停機與BUTTON2。
獨立唯讀 verifier 發現時間戳和 continuous-stop 問題；兩項已修正，
其重新查閱確認修正與策略 assertions。實機校準、制動、比賽勝率未驗證。

2026-10-13 遷移前本地 gate：Arduino CLI **1.4.1**、隔離的 Arduino-ESP32
**3.3.8**、Pololu **1.3.1**，repository validator 成功編譯全部 **3** 個
sketch。新 competition sketch 使用 376569 bytes flash、24516 bytes RAM，
沒有新 sketch 自身的 compiler warnings；ESP32 core TinyUSB 和舊測試的
既有 warnings 仍存在。不是 GitHub Actions 的遠端執行結果。
兩組 host assertions、`git diff --check`、新檔空白、local Markdown links
與固定 workflow contract 都通過。隔離回復舊時間戳順序會令校準 assertion
失敗，證實回歸測試能抓到原問題。

最終 primary 架構評估已對照策略、adapter、實際編譯和 host 證據：停止／
避邊優先序、單次曝光序列、啟動互鎖和校準時序的重大 findings 已解決。
軟件交付完成；機械尺寸、前臉接觸、感測器擺位、FG電平、馬達方向、
煞停距離和比賽成效需要實機證據。未 commit、push、merge 或 upload。

### 技術來源

- [Pololu carrier documentation](https://www.pololu.com/product/2490)：地址、
  多頭 XSHUT、電氣邏輯和環境限制。實際 clone 模組須另核對。
- [Pololu VL53L0X 1.3.1 source](https://github.com/pololu/vl53l0x-arduino/blob/1.3.1/VL53L0X.cpp)：
  非阻塞 single-shot 啟動取自 `readRangeSingleMillimeters()`。
- [ST UM2039](https://www.st.com/resource/en/user_manual/dm00279088-world-smallest-timeofflight-ranging-and-gesture-detection-sensor-application-programming-interface-stmicroelectronics.pdf)：
  單次／連續測距和停止流程；停止連續量測不是立即完成。
- [Espressif LEDC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html)：
  指定 PWM頻率和 duty。實際編譯使用 pinned 3.3.8，而非文件 latest 版本。
