# THIẾT KẾ HỆ THỐNG — Tưới cây thông minh đa vùng (ESP32 + ESP-NOW + AI tại server cục bộ)

> Đồ án 2 — Nguyễn Quốc Hưng (23139019), Nguyễn Quốc Khánh (23139022)
> Tài liệu kỹ thuật nội bộ, dùng làm căn cứ để lập trình firmware, server và AI.
> Nguồn: `Ke_Hoach_Thuc_Hien_DoAn2_Tuoi_Cay_Thong_Minh_ESPNOW.docx`

---

## 1. Tổng quan

Hệ thống gồm **3 nút Zone** (mỗi nút: ESP32 + cảm biến độ ẩm đất điện dung + SHT30 + relay bơm) gửi dữ liệu không dây bằng **ESP-NOW** tới **1 ESP32 Gateway**. Gateway nối **USB-Serial** với **server cục bộ (laptop)**. Server chạy:

- Backend **Python/FastAPI**: đọc Serial, xử lý dữ liệu, ra quyết định tưới.
- **MongoDB**: lưu dữ liệu cảm biến, lịch sử tưới, kết quả dự đoán.
- **AI**: Random Forest / XGBoost dự đoán nhu cầu tưới và thời lượng tưới.
- **Web dashboard**: giám sát realtime và điều khiển thủ công.

### 1.1 Nguyên tắc thiết kế

| Nguyên tắc | Cụ thể |
|---|---|
| Server là "bộ não" | Quyết định tưới (AI / ngưỡng / thủ công) nằm ở server. Nút chỉ thi hành lệnh. |
| Nút tự bảo vệ | Nút luôn áp giới hạn thời gian bơm tối đa và cooldown, bất kể lệnh từ đâu. |
| Hoạt động khi mất kết nối | Mất liên lạc với gateway quá 5 phút thì nút tự tưới theo ngưỡng (fallback). |
| Đơn giản, debug được | Gateway ↔ Server dùng JSON lines; bảng MAC ghi cứng; một firmware dùng chung cho 3 nút. |

### 1.2 Phạm vi

- **Trong phạm vi**: 3 Zone, 1 gateway, 1 server laptop, mạng LAN/localhost.
- **Ngoài phạm vi**: truy cập Internet/cloud, đăng nhập người dùng, OTA firmware, nút chạy pin/deep sleep.

---

## 2. Kiến trúc tổng thể

```
 ┌──────────── Zone 1 ───────────┐
 │ Soil(ADC) SHT30(I2C) Relay→Bơm │
 │        ESP32 DevKit V1         │──┐
 └────────────────────────────────┘  │
 ┌──────────── Zone 2 ───────────┐   │  ESP-NOW (2.4 GHz, kênh 1)
 │            ...                 │──┤  unicast + ACK
 └────────────────────────────────┘  │
 ┌──────────── Zone 3 ───────────┐   │
 │            ...                 │──┘
 └────────────────────────────────┘  │
                                     ▼
                         ┌───────────────────────┐
                         │  ESP32 Gateway         │
                         │  (ESP-NOW ⇄ Serial)    │
                         └───────────┬───────────┘
                                     │ USB-Serial 115200, JSON lines
                                     ▼
 ┌──────────────────────── Server cục bộ (Laptop) ────────────────────────┐
 │  serial_bridge ─► ingest ─► MongoDB ◄── decision engine (AI/ngưỡng)     │
 │        ▲                       │              │                         │
 │        └──── command manager ◄─┴──────────────┘                         │
 │  FastAPI REST + WebSocket ─► Web dashboard (HTML/JS + Chart.js)         │
 └─────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Luồng dữ liệu chính

1. **Đo**: Nút đọc cảm biến mỗi 1 giây, lọc, và cứ 30 giây gửi `TELEMETRY` lên gateway.
2. **Chuyển tiếp**: Gateway nhận gói, gắn RSSI, rồi in 1 dòng JSON ra Serial.
3. **Lưu**: Server gán timestamp, lưu vào `readings`, đẩy lên WebSocket.
4. **Quyết định**: Mỗi 5 phút, decision engine chạy cho từng Zone theo chế độ của Zone đó (AI / THRESHOLD / MANUAL).
5. **Ra lệnh**: Server gửi `{"t":"cmd",...}` xuống gateway. Gateway gửi `CMD_WATER` cho nút, có retry.
6. **Thi hành**: Nút bật bơm, trả `ACK`, và gửi `PUMP_EVENT` khi bơm bật/tắt.
7. **Phản hồi**: Các telemetry tiếp theo cho thấy độ ẩm sau tưới, khép vòng *đo → dự đoán → tưới → đo phản hồi*.

---

## 3. Phần cứng

> Sơ đồ nguyên lý chi tiết, netlist, quy trình hiệu chuẩn: [HARDWARE.md](HARDWARE.md)

### 3.1 Danh sách linh kiện (BOM)

| # | Linh kiện | SL | Ghi chú |
|---|---|---|---|
| 1 | ESP32 DevKit V1 (ESP32-WROOM-32) | 4 | 3 nút Zone + 1 gateway |
| 2 | Cảm biến độ ẩm đất điện dung v1.2/v2.0 | 3 | Ngõ ra analog, cấp 3.3V |
| 3 | Cảm biến SHT30 (module I2C) | 3 | Địa chỉ 0x44 |
| 4 | Module relay 1 kênh 5V, có opto, kích mức LOW | 3 | |
| 5 | Bơm chìm mini 5V DC | 3 | Mỗi Zone một bơm và một bình nước riêng |
| 6 | Adapter 5V/2A | 3 | Mỗi Zone một adapter, cấp cho bơm + ESP32 |
| 7 | Ống dẫn nước silicon, khay trồng | 3 bộ | 3 khay cùng loại cây, cùng loại đất |
| 8 | Tụ 470–1000 µF/10V | 3 | Gắn gần bơm để chống sụt áp |
| 9 | Breadboard / PCB đục lỗ, dây, terminal | — | |
| 10 | Cáp micro-USB (data) | 1 | Gateway ↔ laptop |

### 3.2 Gán chân — nút Zone (ESP32 DevKit V1)

| Chức năng | GPIO | Ghi chú |
|---|---|---|
| Cảm biến độ ẩm đất (AOUT) | **GPIO34** | ADC1_CH6, chỉ nhận đầu vào, không xung đột Wi-Fi. Attenuation 11 dB. |
| SHT30 SDA | **GPIO21** | I2C mặc định, pull-up có sẵn trên module |
| SHT30 SCL | **GPIO22** | |
| Relay IN | **GPIO26** | Kích mức LOW. Khởi động ở trạng thái **HIGH (OFF)**. |
| LED trạng thái | GPIO2 | LED onboard |

- Tránh dùng các chân strapping (0, 2, 12, 15) cho relay để bơm không tự bật lúc khởi động. GPIO2 chỉ dùng cho LED.
- **Không** dùng ADC2 vì ADC2 không hoạt động khi bật Wi-Fi/ESP-NOW.

### 3.3 Sơ đồ nguồn — nút Zone

```
Adapter 5V/2A ──┬── VIN ESP32 (LDO onboard → 3.3V → Soil sensor, SHT30)
                ├── VCC relay module
                └── COM relay ─ NO ─► (+) Bơm 5V (−) ─► GND
                                     tụ 1000µF song song bơm
GND chung: adapter, ESP32, relay, cảm biến
```

Lưu ý: khi nạp code qua USB thì không cấp đồng thời VIN, hoặc dùng diode để tránh dòng ngược. Đặt cảm biến xa bơm và relay, chạy dây cảm biến xa dây bơm để giảm nhiễu ADC.

### 3.4 Gateway

Một ESP32 DevKit V1 cắm USB vào laptop, lấy nguồn từ USB, không gắn ngoại vi. LED GPIO2 nháy mỗi khi nhận hoặc gửi gói.

---

## 4. Firmware

### 4.1 Công cụ và cấu trúc

- **ESP-IDF v5.5.5** (C), cài tại `D:\esp\esp-idf`, toolchain ở `D:\esp\.espressif`. Các task tách riêng bằng FreeRTOS (`xTaskCreatePinnedToCore`, queue, task notification). `CONFIG_FREERTOS_HZ=1000`.
- Hai project IDF: `firmware/node` và `firmware/gateway`, dùng chung component `firmware/components/common`:
  - `protocol.h`: struct gói tin ESP-NOW.
  - `app_config.h`: bảng MAC, kênh Wi-Fi, chân GPIO, hằng số thời gian.
- Driver: `esp_adc` (oneshot, ADC1), `i2c_master` (driver SHT30 tự viết, kiểm CRC-8), `esp_now`, `esp_console` (REPL trên nút), `cJSON` (gateway).
- **Một firmware cho cả 3 nút**: Zone ID đặt bằng lệnh console `zone <1-3>` và lưu trong NVS. Chưa đặt zone thì nút không gửi/nhận ESP-NOW.
- Mở môi trường build: `. firmware\idf_env.ps1` (PowerShell), rồi `idf.py build flash monitor` trong `firmware/node` hoặc `firmware/gateway`.

```c
// app_config.h (trích)
#define ESPNOW_CHANNEL        1
static const uint8_t GATEWAY_MAC[6]    = {0x24,0x6F,0x28,0x00,0x00,0x00};
static const uint8_t NODE_MAC[3][6]    = { {...}, {...}, {...} }; // Zone 1..3

#define TELEMETRY_PERIOD_MS   30000
#define SENSOR_PERIOD_MS      1000
#define SHT_PERIOD_MS         5000
#define NODE_OFFLINE_MS       90000    // gateway: 3 chu kỳ không nhận
#define BEACON_PERIOD_MS      60000
#define FALLBACK_TIMEOUT_MS   300000   // node: 5 phút không nghe gateway
#define CMD_RETRY_MAX         3
#define CMD_ACK_TIMEOUT_MS    300
#define PUMP_MAX_S            30
#define PUMP_COOLDOWN_MS      600000   // 10 phút
#define FALLBACK_WATER_S      10
```

### 4.2 Firmware nút Zone (dùng chung cho 3 nút)

| Task | Chu kỳ | Nhiệm vụ |
|---|---|---|
| `sensorTask` | 1 s | Đọc ADC (16 mẫu → median 5 → EMA α=0.2), quy đổi %; đọc SHT30 mỗi 5 s; ghi lỗi nếu SHT30 không phản hồi. |
| `commTask` | 30 s + sự kiện | Gửi `TELEMETRY`; xử lý queue RX (`CMD_WATER`, `CMD_STOP`, `CONFIG`, `BEACON`); gửi `ACK`, `PUMP_EVENT`. |
| `pumpTask` | 100 ms | Máy trạng thái bơm, áp giới hạn an toàn, tắt bơm khi hết giờ. |
| `supervisorTask` | 1 s | Theo dõi `last_gateway_rx`, chuyển NORMAL ⇄ FALLBACK; điều khiển LED. |

**Hiệu chuẩn độ ẩm đất**: mỗi nút lưu `ADC_DRY` (cảm biến để trong không khí) và `ADC_WET` (nhúng nước) trong NVS (`Preferences`), đặt bằng lệnh Serial `cal dry` / `cal wet` khi lắp ráp.

```
soil_pct = clamp( (ADC_DRY - raw) * 100 / (ADC_DRY - ADC_WET), 0, 100 )
```

**Máy trạng thái bơm**

```
IDLE ──CMD_WATER(dur) / fallback──► (kiểm tra cooldown, dur ≤ PUMP_MAX_S) ──► RUNNING
RUNNING ──hết dur / CMD_STOP / quá PUMP_MAX_S──► IDLE (ghi last_water_end, gửi PUMP_EVENT off)
Lệnh bị từ chối (đang cooldown / đang chạy) → ACK status = REJECTED_*
```

**Chế độ vận hành của nút**

| Chế độ | Điều kiện vào | Hành vi |
|---|---|---|
| `NORMAL` | Nhận bất kỳ gói nào từ gateway (beacon, lệnh, config) | Chỉ tưới khi có lệnh |
| `FALLBACK` | `millis() - last_gateway_rx > 5 phút` | Nếu `soil_pct < soil_low` và hết cooldown thì tưới `FALLBACK_WATER_S` giây (nguồn = FALLBACK). Vẫn gửi telemetry bình thường. |

`soil_low` cho fallback được server đồng bộ xuống bằng gói `CONFIG` và lưu trong NVS.

### 4.3 Firmware Gateway

| Task | Nhiệm vụ |
|---|---|
| ESP-NOW RX callback | Đẩy gói + RSSI + MAC nguồn vào `rxQueue` (không xử lý nặng trong callback). |
| `uplinkTask` | Lấy gói từ `rxQueue`, kiểm tra MAC thuộc bảng, cập nhật `last_seen[zone]`, in JSON ra Serial. |
| `serialRxTask` | Đọc từng dòng từ Serial, parse JSON, rồi xếp lệnh vào `cmdQueue[zone]`. |
| `cmdTask` | Gửi lệnh, chờ ACK (300 ms), retry ≤ 3 lần; báo kết quả `ok` / `rejected` / `timeout` lên server. |
| `livenessTask` (1 s) | Nếu `now - last_seen[zone] > 90 s` thì phát `{"t":"node","zone":z,"online":false}` (chỉ phát khi trạng thái đổi). |
| `beaconTimer` (60 s) | Gửi `BEACON` (kèm epoch nếu server đã đồng bộ giờ) tới cả 3 nút, **chỉ khi server còn sống** (nhận dòng JSON từ server trong 3 phút gần nhất). Laptop tắt nhưng gateway vẫn có nguồn thì nút vẫn chuyển được sang FALLBACK. LED GPIO2 sáng khi server còn sống. |

Gateway **không đệm dữ liệu** khi laptop mất kết nối: dữ liệu trong khoảng đó bị bỏ qua.

---

## 5. Giao thức ESP-NOW (Node ⇄ Gateway)

> Bảng offset từng byte, ví dụ hex, sơ đồ tuần tự ACK: [PROTOCOL.md](PROTOCOL.md)

- Unicast theo MAC trong bảng ghi cứng, kênh 1, không mã hóa (có thể bật PMK/LMK sau).
- Lớp MAC của ESP-NOW đã có ACK. Ngoài ra còn **ACK tầng ứng dụng** cho lệnh, để xác nhận nút đã *chấp nhận và thi hành*.
- Mọi gói đều có header chung. `seq` 16-bit tăng dần theo từng chiều gửi.

```c
#pragma pack(push, 1)
enum MsgType : uint8_t {
  MSG_TELEMETRY  = 1,  // node → gw
  MSG_PUMP_EVENT = 2,  // node → gw
  MSG_ACK        = 3,  // node → gw
  MSG_CMD_WATER  = 10, // gw → node
  MSG_CMD_STOP   = 11, // gw → node
  MSG_CONFIG     = 12, // gw → node
  MSG_BEACON     = 13, // gw → node
};

typedef struct {
  uint8_t  type;
  uint8_t  zone_id;    // 1..3
  uint16_t seq;
} MsgHeader;           // 4 byte

typedef struct {
  MsgHeader h;
  uint16_t soil_raw;   // ADC thô
  uint16_t soil_x10;   // % độ ẩm đất × 10
  int16_t  temp_x100;  // °C × 100
  uint16_t hum_x100;   // %RH × 100
  uint8_t  pump_on;    // 0/1
  uint8_t  node_mode;  // 0=NORMAL, 1=FALLBACK
  uint8_t  flags;      // bit0: lỗi SHT30, bit1: lỗi ADC, bit2: đang cooldown
  uint32_t uptime_s;
} MsgTelemetry;        // 19 byte

typedef struct {
  MsgHeader h;
  uint8_t  on;         // 1=bật, 0=tắt
  uint8_t  source;     // 0=CMD, 1=FALLBACK, 2=SAFETY_STOP
  uint16_t cmd_seq;    // seq lệnh gây ra (0 nếu fallback)
  uint16_t duration_s; // thời lượng đã chạy (khi on=0)
} MsgPumpEvent;

typedef struct {
  MsgHeader h;
  uint16_t ack_seq;    // seq của lệnh được xác nhận
  uint8_t  status;     // 0=OK, 1=REJ_COOLDOWN, 2=REJ_BUSY, 3=REJ_INVALID
} MsgAck;

typedef struct {
  MsgHeader h;
  uint16_t duration_s; // 1..PUMP_MAX_S
} MsgCmdWater;

typedef struct { MsgHeader h; } MsgCmdStop;

typedef struct {
  MsgHeader h;
  uint16_t soil_low_x10;   // ngưỡng fallback
  uint16_t pump_max_s;
  uint16_t fallback_water_s;
} MsgConfig;

typedef struct {
  MsgHeader h;
  uint32_t epoch;          // 0 nếu chưa đồng bộ
} MsgBeacon;
#pragma pack(pop)
```

### 5.1 Quy tắc tin cậy

| Tình huống | Xử lý |
|---|---|
| Lệnh (`CMD_*`, `CONFIG`) | Gateway gửi và chờ `MsgAck.ack_seq == seq` trong 300 ms, retry tối đa 3 lần, sau đó báo `timeout`. |
| Lệnh trùng (do retry) | Nút nhớ `last_cmd_seq`. Gặp seq trùng thì **gửi lại ACK cũ, không bơm lần 2**. |
| Telemetry / PumpEvent | Dựa vào ACK lớp MAC (`esp_now_send` callback). Lỗi thì retry ngay tối đa 2 lần, sau đó bỏ (telemetry kế tiếp sẽ bù). PumpEvent vẫn hiện trong telemetry (`pump_on`). |
| Nút offline | Gateway: 90 s không nhận gói nào. |
| Gateway mất | Nút: 5 phút không nhận gói nào (beacon 60 s), chuyển sang FALLBACK. |

---

## 6. Giao thức Serial (Gateway ⇄ Server)

USB-Serial **115200 8N1**, mỗi bản tin là **1 dòng JSON** kết thúc bằng `\n`. Server **chỉ xử lý dòng bắt đầu bằng `{`**, bỏ qua các dòng khác (log ESP-IDF mức WARN/ERROR, boot ROM, dòng chẩn đoán `# ...` của gateway).

### 6.1 Gateway → Server

```json
{"t":"hello","fw":"gw-1.0","mac":"24:6F:28:xx:xx:xx"}
{"t":"tel","zone":1,"seq":120,"soil":43.2,"soil_raw":2345,"temp":28.41,"hum":71.20,"pump":0,"mode":"normal","flags":0,"up":3600,"rssi":-58}
{"t":"pump","zone":2,"on":1,"src":"cmd","cmd_seq":7,"dur":0}
{"t":"pump","zone":2,"on":0,"src":"cmd","cmd_seq":7,"dur":15}
{"t":"ack","zone":2,"seq":7,"status":"ok"}          // ok | rej_cooldown | rej_busy | rej_invalid | timeout
{"t":"node","zone":3,"online":false}
```

### 6.2 Server → Gateway

```json
{"t":"cmd","zone":2,"seq":7,"dur":15}
{"t":"stop","zone":2,"seq":8}
{"t":"cfg","zone":1,"seq":9,"low":35.0,"max":30,"fb":10}
{"t":"time","epoch":1794000000}     // gửi mỗi 60 s: đồng bộ giờ + heartbeat của server
{"t":"ping"}                         // heartbeat không kèm giờ (tuỳ chọn)
```

`seq` cho lệnh do **server cấp** (mỗi Zone một bộ đếm riêng, lưu bền trong `commands` để không bắt đầu lại từ 1 khi server khởi động lại) và gateway giữ nguyên khi gửi xuống nút, nhờ vậy server đối chiếu được ACK với bản ghi trong `commands`. Gateway trả `status: "busy"` nếu hàng đợi lệnh đầy.

Công cụ thử khi chưa có server: `python server/tools/gw_test.py COM5`.

---

## 7. Server cục bộ

### 7.1 Công nghệ

- Python 3.11+, **FastAPI** + Uvicorn, **motor** (MongoDB async), **pyserial**, scikit-learn, xgboost, joblib, pandas.
- Chạy trên laptop. Trong giai đoạn thu dữ liệu: **tắt chế độ ngủ/tắt màn hình khi cắm sạc**, bắt đầu từ tuần 12.
- Cấu hình bằng file `.env`: `SERIAL_PORT=COM5`, `MONGO_URI=mongodb://localhost:27017`, `DB_NAME=irrigation`.

### 7.2 Các module

| Module | Vai trò |
|---|---|
| `serial_bridge.py` | Thread đọc Serial, đưa từng dòng vào `asyncio.Queue`; ghi lệnh xuống. **Tự kết nối lại** cổng COM mỗi 5 s khi bị rút cáp hoặc lỗi. Khi kết nối lại thì gửi `time` và `cfg` cho cả 3 Zone. |
| `ingest.py` | Parse JSON, gán `ts = now()` (giờ server là chuẩn), ghi MongoDB, cập nhật trạng thái Zone trong bộ nhớ, phát lên WebSocket. |
| `decision.py` | Scheduler 5 phút/Zone. Theo `mode` của Zone, gọi `ThresholdPolicy` hoặc `AIPolicy`, lưu `predictions`, rồi gửi lệnh nếu cần. |
| `commands.py` | Cấp `seq`, lưu `commands` (status `pending`), cập nhật khi nhận `ack`. Áp cooldown phía server. |
| `ml_runtime.py` | Nạp `ml/models/*.joblib`, tạo vector đặc trưng từ MongoDB (cùng hàm với lúc train), trả về `p_need`, `need`, `dur`. |
| `api.py`, `ws.py` | REST API + WebSocket cho dashboard. |
| `static/` | Dashboard HTML/JS + Chart.js. |

### 7.3 Chế độ Zone và chính sách quyết định

| Chế độ | Hành vi mỗi 5 phút |
|---|---|
| `MANUAL` | Không tự tưới. Chỉ tưới khi người dùng bấm trên dashboard. |
| `THRESHOLD` | Nếu `soil < soil_low` và hết cooldown thì tưới `dur`. `dur` = `threshold_dur` cố định, hoặc **ngẫu nhiên trong [5, 25] s** khi `collect_mode=true` (để có dữ liệu đa dạng cho mô hình thời lượng, xem §9). |
| `AI` | `p_need = clf(x)`. Nếu `p_need ≥ 0.5` và hết cooldown thì `dur = clip(reg(x), 3, pump_max_s)`, rồi gửi lệnh. |

Mọi quyết định (kể cả "không tưới") đều ghi vào `predictions` để vẽ lên dashboard và phục vụ đánh giá.

Không ra quyết định cho Zone đang offline, hoặc Zone có dữ liệu cũ hơn 2 phút.

---

## 8. Cơ sở dữ liệu — MongoDB

MongoDB Community cài local, database `irrigation`.

### 8.1 `readings` (time-series collection)

```js
db.createCollection("readings", {
  timeseries: { timeField: "ts", metaField: "zone_id", granularity: "seconds" }
})
```

```json
{ "ts": ISODate, "zone_id": 1, "soil": 43.2, "soil_raw": 2345, "temp": 28.41,
  "hum": 71.2, "pump": 0, "node_mode": "normal", "flags": 0, "rssi": -58, "seq": 120 }
```

### 8.2 Collection thường

| Collection | Trường chính | Index |
|---|---|---|
| `irrigation_events` | `zone_id, start_ts, end_ts, duration_s, source (cmd/fallback/manual), decided_by (ai/threshold/manual/node), cmd_seq, soil_before, soil_after_10m` | `{zone_id:1, start_ts:-1}` |
| `commands` | `zone_id, seq, type (water/stop/cfg), dur, created_ts, status (pending/ok/rej_*/timeout), ack_ts, decided_by` | `{zone_id:1, seq:1}` unique |
| `predictions` | `ts, zone_id, mode, features{}, p_need, need, dur_pred, action_taken, model_version` | `{zone_id:1, ts:-1}` |
| `zones` | `_id: zone_id, name, mode, soil_low, soil_target, pump_max_s, cooldown_s, threshold_dur, collect_mode, pump_flow_ml_s, online, last_seen` | — |
| `node_status` | `ts, zone_id, online` (lịch sử online/offline) | `{zone_id:1, ts:-1}` |
| `server_sessions` | `start_ts, end_ts` của mỗi phiên kết nối Serial, dùng để xác định khoảng trống dữ liệu khi làm sạch | — |

`soil_after_10m` được một job điền vào 10 phút sau khi bơm tắt, là độ ẩm trung bình trong cửa sổ [+8, +12] phút.

---

## 9. AI dự đoán nhu cầu tưới

### 9.1 Bài toán: mô hình hai tầng

| Tầng | Loại | Đầu ra | Thuật toán | Metric |
|---|---|---|---|---|
| 1 | Phân loại | `need_water` ∈ {0,1}: có cần tưới bây giờ không | Random Forest vs XGBoost | Accuracy, Precision, Recall, **F1** |
| 2 | Hồi quy | `dur` (giây) cần tưới để đạt `soil_target` | RF Regressor vs XGBoost Regressor | **MAE** (giây), MAE quy ra Δđộ ẩm (%) |

### 9.2 Gán nhãn

Dữ liệu được thu ở chế độ **THRESHOLD** với `collect_mode=true`. Nhãn **không** lấy từ quyết định của bộ ngưỡng (vì như vậy mô hình chỉ học lại cái ngưỡng), mà lấy từ **tương lai thực tế**:

- **Tầng 1**: tại mỗi mốc 5 phút `t` của Zone `z`:
  `need_water(t) = 1` nếu trong `(t, t + H]`, với **H = 2 giờ**, độ ẩm đất giảm xuống dưới `soil_low` **hoặc** có một lần tưới bắt đầu. Ngược lại `need_water(t) = 0`.
  Loại các mẫu: trong vòng 30 phút sau một lần tưới (độ ẩm chưa ổn định), có khoảng trống dữ liệu, hoặc cửa sổ tương lai không đủ H.
  → Mô hình học cách **tưới sớm, dự đoán trước**, thay vì đợi chạm ngưỡng như bộ ngưỡng.
- **Tầng 2**: mỗi bản ghi `irrigation_events` là 1 mẫu:
  đầu vào gồm `soil_before`, `delta_target = soil_after_10m − soil_before`, `temp`, `hum`, `zone_id`; nhãn là `duration_s` thực tế.
  Khi suy luận: `delta_target = soil_target − soil_now`, mô hình trả về số giây cần bơm.
  Đó là lý do chế độ thu thập dùng thời lượng **ngẫu nhiên 5–25 s**: nếu thời lượng cố định thì không học được quan hệ.

### 9.3 Đặc trưng (dùng chung một hàm `build_features()` cho train và runtime)

| Nhóm | Đặc trưng |
|---|---|
| Hiện tại | `soil`, `temp`, `hum` (trung bình 5 phút gần nhất) |
| Xu hướng | `dsoil_30m`, `dsoil_60m` (độ dốc %/giờ), `dtemp_60m` |
| Thời gian | `hour_sin`, `hour_cos` |
| Lịch sử tưới | `mins_since_last_water`, `last_water_dur`, `waters_last_24h` |
| Zone | `zone_id` (one-hot) |

### 9.4 Huấn luyện và đánh giá

- Làm sạch: loại các điểm có `flags` lỗi, các bước nhảy độ ẩm > 15%/30 s không do tưới, và các khoảng trống (từ `server_sessions`).
- Chia **theo thời gian** (không xáo trộn) **70 / 15 / 15** Train / Validation / Test để tránh rò rỉ thông tin.
- Tuning trên tập Validation (grid nhỏ: `n_estimators`, `max_depth`, `learning_rate`). Mất cân bằng lớp xử lý bằng `class_weight` / `scale_pos_weight`.
- Tập Test: báo cáo Accuracy, F1 và ma trận nhầm lẫn (tầng 1); MAE (tầng 2); so với **baseline ngưỡng** (dự đoán `need = soil < soil_low` tại thời điểm t).
- Lưu `ml/models/clf_vX.joblib`, `reg_vX.joblib` kèm `meta.json` (danh sách feature, version, metric).
- Thực hiện trong `ml/notebooks/` và `ml/train.py`. `ml/features.py` được import dùng chung với server.

### 9.5 Thực nghiệm so sánh AI và ngưỡng (tuần 16)

- **Thiết kế chéo**: pha 1 (2–3 ngày): Z1 = AI, Z2 = THRESHOLD, Z3 = AI. Pha 2 (2–3 ngày): Z1 = THRESHOLD, Z2 = AI, Z3 = THRESHOLD.
- Trong thực nghiệm, THRESHOLD dùng thời lượng **cố định** (`collect_mode=false`).
- Chỉ số so sánh cho mỗi Zone và mỗi pha:
  - Tổng lượng nước = Σ `duration_s` × `pump_flow_ml_s` (đo lưu lượng bơm bằng cốc đong).
  - Độ lệch chuẩn của độ ẩm đất.
  - % thời gian độ ẩm nằm trong vùng [`soil_low`, 75%].
  - Số lần tưới.

---

## 10. Web dashboard

Trang duy nhất `http://localhost:8000/`, viết bằng HTML/JS thuần + Chart.js, nhận cập nhật realtime qua WebSocket. Không cần đăng nhập.

- **3 thẻ Zone**: độ ẩm đất (%), nhiệt độ, độ ẩm không khí, trạng thái bơm, online/offline (thời điểm thấy lần cuối), chế độ node (normal/fallback), RSSI, chế độ Zone, kết quả AI gần nhất (`p_need`, `dur_pred`).
- **Biểu đồ lịch sử**: độ ẩm đất của 3 Zone theo thời gian (chọn 6 giờ / 24 giờ / 7 ngày), có đánh dấu các lần tưới; biểu đồ nhiệt độ và độ ẩm không khí.
- **Điều khiển**: nút "Tưới N giây", nút "Dừng"; chọn chế độ (MANUAL / THRESHOLD / AI); form chỉnh `soil_low`, `soil_target`, `pump_max_s`, `threshold_dur`, `collect_mode`.
- **Nhật ký**: các lệnh gần nhất cùng trạng thái ACK, các sự kiện offline.

### 10.1 REST API

| Method | Endpoint | Mô tả |
|---|---|---|
| GET | `/api/zones` | Trạng thái hiện tại + cấu hình của 3 Zone |
| PUT | `/api/zones/{id}` | Cập nhật cấu hình/chế độ (tự gửi `cfg` xuống nút) |
| POST | `/api/zones/{id}/water` | Body `{"dur": 15}`: tưới thủ công |
| POST | `/api/zones/{id}/stop` | Dừng bơm |
| GET | `/api/readings?zone=&from=&to=&bucket=` | Lịch sử cảm biến (có gom nhóm theo bucket) |
| GET | `/api/events?zone=&from=&to=` | Lịch sử tưới |
| GET | `/api/predictions?zone=&from=&to=` | Lịch sử dự đoán |
| GET | `/api/commands?limit=50` | Lệnh gần đây |
| GET | `/api/stats?zone=&from=&to=` | Chỉ số so sánh (§9.5) |
| WS | `/ws` | Đẩy `tel`, `pump`, `ack`, `node`, `prediction` |

---

## 11. Cơ chế an toàn (tổng hợp)

| Lớp | Cơ chế |
|---|---|
| Nút (cứng) | Bơm tối đa `PUMP_MAX_S = 30 s` mỗi lần. Cooldown 10 phút. Relay mặc định OFF khi boot/reset. Bỏ qua lệnh trùng `seq`. Mất SHT30 vẫn chạy (gửi kèm flag). Mất cảm biến đất (ADC ngoài khoảng hiệu chuẩn) thì **không** tưới fallback. |
| Nút (fallback) | 5 phút mất gateway thì tưới theo ngưỡng cục bộ, `FALLBACK_WATER_S = 10 s`. |
| Gateway | Phát hiện nút offline sau 90 s. Báo lệnh `timeout` sau 3 lần retry. |
| Server | Cooldown phía server. Không quyết định khi dữ liệu cũ hoặc nút offline. Clip `dur` ∈ [3, `pump_max_s`]. Tự kết nối lại Serial. |
| Watchdog | Bật Task WDT trên nút và gateway. |

Các con số 30 s và 10 s sẽ chỉnh lại sau khi đo lưu lượng bơm thực tế (tuần 10–12).

---

## 12. Kế hoạch kiểm thử

| Mức | Nội dung | Tiêu chí đạt |
|---|---|---|
| Đơn vị (nút) | Đọc ADC, hiệu chuẩn, SHT30, relay | Độ ẩm khô ≈ 0%, ướt ≈ 100%; nhiễu sau lọc < ±1% |
| ESP-NOW | Gửi/nhận 3 nút ↔ gateway, ACK/retry, lệnh trùng seq | Lệnh trùng không bơm 2 lần; tỉ lệ nhận > 99% ở khoảng cách 5 m |
| Tầm truyền | Đo tỉ lệ mất gói theo khoảng cách (5/10/20/30 m, có/không vật cản) | Bảng tỉ lệ mất gói + RSSI |
| Mất liên lạc | Rút gateway, đợi > 5 phút | Nút vào FALLBACK và tưới đúng ngưỡng; cắm lại thì về NORMAL |
| Nút offline | Rút nguồn 1 nút | Dashboard báo offline sau ≤ 90 s |
| Server | Rút/cắm USB gateway, khởi động lại server | Tự kết nối lại, không crash, ghi `server_sessions` |
| Toàn hệ thống | Vòng lặp đo → dự đoán → tưới → đo phản hồi trong ≥ 24 giờ | Không tưới quá giới hạn; dữ liệu liên tục |
| Demo | 3 kịch bản: Zone khô / trung bình / đủ ẩm | AI tưới đúng Zone khô, không tưới Zone đủ ẩm |

---

## 13. Cấu trúc repo

```
doan2/
├── firmware/
│   ├── idf_env.ps1             # nạp môi trường ESP-IDF (D:\esp)
│   ├── components/common/      # protocol.h, app_config.h (MAC, chân, hằng số)
│   ├── node/                   # project IDF: main.c, sensor.c, sht30.c, pump.c, comm.c, console.c, settings.c
│   └── gateway/                # project IDF: main.c, uplink.c, cmd.c
├── server/
│   ├── app/
│   │   ├── main.py             # FastAPI app, khởi động các task
│   │   ├── serial_bridge.py
│   │   ├── ingest.py
│   │   ├── decision.py
│   │   ├── commands.py
│   │   ├── ml_runtime.py
│   │   ├── db.py
│   │   ├── api.py
│   │   └── ws.py
│   ├── static/                 # index.html, app.js, style.css
│   ├── requirements.txt
│   └── .env.example
├── ml/
│   ├── features.py             # build_features() — dùng chung với server
│   ├── label.py                # gán nhãn need_water, mẫu duration
│   ├── train.py
│   ├── notebooks/
│   └── models/
└── docs/
    └── DESIGN.md
```

---

## 14. Lộ trình theo tuần (ánh xạ kế hoạch → hạng mục thiết kế)

| Tuần | Hạng mục | Sản phẩm |
|---|---|---|
| 8 (05–11/10) | §2, §3.1, §5 | Sơ đồ khối, BOM, khung gói tin |
| 9 (12–18/10) | §3.2–3.4 | Sơ đồ nguyên lý nút; mua linh kiện, 3 khay |
| 10 (19–25/10) | §4.2 | Firmware nút (sensor, lọc, hiệu chuẩn, bơm); lấy MAC |
| 11 (26/10–01/11) | §4.3, §5, §6, §11 | ESP-NOW + ACK/retry, gateway ⇄ Serial, fallback, heartbeat |
| 12 (02–08/11) | §7, §8 | Server + MongoDB, lắp 3 Zone, **bắt đầu thu dữ liệu** (`THRESHOLD`, `collect_mode=true`) |
| 13 (09–15/11) | §10 | Dashboard bản đầu; tiếp tục thu dữ liệu |
| 14 (16–22/11) | §9.2–9.4 | Làm sạch, gán nhãn, đặc trưng, train RF/XGBoost |
| 15 (23–29/11) | §9.4, §7.3 | Bảng đánh giá; triển khai `AIPolicy` lên server |
| 16 (30/11–06/12) | §9.5, §12 | Thực nghiệm chéo AI vs ngưỡng; test offline, tầm truyền, mất gói |
| 17 (07–13/12) | §12 (demo) | Bản nháp báo cáo; kịch bản demo |
| 18 (14–20/12) | — | Báo cáo, slide, video demo; nộp |

---

## 15. Các quyết định đã chốt (tóm tắt)

| # | Quyết định |
|---|---|
| Q1 | Tài liệu kỹ thuật nội bộ, `docs/DESIGN.md` |
| Q2 | ESP32 DevKit V1, **ESP-IDF v5.5.5** (C), task FreeRTOS |
| Q3 | Python + FastAPI + WebSocket, frontend HTML/JS + Chart.js |
| Q4 | MongoDB (local Community, `motor`, `readings` là time-series) |
| Q5 | Server chạy trên laptop, tắt chế độ ngủ khi thu dữ liệu |
| Q6 | AI hai tầng: phân loại cần tưới + hồi quy thời lượng |
| Q7 | Bơm chìm 5V riêng mỗi Zone, relay, adapter 5V/2A, không deep sleep |
| Q8 | Server quyết định (AI/THRESHOLD/MANUAL theo từng Zone), nút fallback khi mất gateway |
| Q10 | Mất kết nối laptop: chấp nhận mất dữ liệu, ghi lại khoảng trống, không đệm |
| Q11 | Serial JSON lines 115200 |
| Q12 | Telemetry 30 s, offline 90 s, ACK 300 ms × 3 retry, fallback 5 phút, beacon 60 s |
| Q13 | Bảng MAC ghi cứng trong `app_config.h`, Zone ID đặt qua console và lưu NVS, kênh 1 |
| Q14 | Soil GPIO34, SHT30 21/22, relay GPIO26 (active-LOW), LED GPIO2 |
| Q15 | Nhãn theo tương lai (H = 2 giờ); thời lượng học từ sự kiện tưới thực tế |
| Q16 | Hiệu chuẩn khô/ướt → %; median + EMA; `soil_low` 35%, `soil_target` 60%; bơm ≤ 30 s; cooldown 10 phút; fallback 10 s |
| Q17 | Dashboard 1 trang, không đăng nhập |
| Q18 | Monorepo `firmware/ server/ ml/ docs/` |
| Q19 | Quyết định mỗi 5 phút/Zone cho cả AI và ngưỡng |
| Q20 | Thực nghiệm chéo, đảo vai trò sau 2–3 ngày |
| Q21 | Timestamp do server gán; nút dùng `millis()` |
