# Sơ đồ nguyên lý — Nút Zone

> Chi tiết cho §3 của [DESIGN.md](DESIGN.md). Dùng làm netlist để vẽ lại bằng EasyEDA/KiCad/Fritzing cho báo cáo.

## 1. Sơ đồ khối

```
            ┌──────────────── Adapter 5V / 2A ────────────────┐
            │                                                  │
            ▼ +5V                                              ▼ GND
   ┌─────────────────┐   ┌───────────────────┐   ┌──────────────────────┐
   │  KHỐI NGUỒN      │   │  KHỐI CHẤP HÀNH    │   │  KHỐI ĐIỀU KHIỂN      │
   │  C1 1000µF       │──►│  Relay module 5V   │◄──│  ESP32 DevKit V1      │
   │  C2 100nF        │   │  D1 1N5819 + bơm   │   │  (LDO 3.3V onboard)   │
   └─────────────────┘   └───────────────────┘   └─────────┬────────────┘
                                                            │ 3.3V
                                       ┌────────────────────┴───────────┐
                                       ▼                                ▼
                            ┌────────────────────┐          ┌────────────────────┐
                            │ Cảm biến độ ẩm đất  │          │ DHT22 (1-wire)      │
                            │ điện dung → GPIO34  │          │ DATA → GPIO27       │
                            └────────────────────┘          └────────────────────┘
```

## 2. Sơ đồ nguyên lý

Vẽ theo từng khối, các khối nối với nhau qua **nhãn net** (`+5V`, `+3V3`, `GND`, `SOIL_AO`, `DHT_DATA`, `RELAY_IN`, `PUMP+`), giống cách đặt net label trong KiCad/EasyEDA.

### 2.1 Khối nguồn

```
 J1 Adapter 5V/2A
   (+) ───────┬──────────┬────────────── +5V
              │          │
         C1 ═╪═ 1000µF  ═╪═ C3 10µF + 100nF (đặt sát chân VIN ESP32)
              │          │
   (−) ───────┴──────────┴────────────── GND
```

### 2.2 Khối điều khiển — ESP32 DevKit V1

```
                 ┌────────────────────────┐
     +5V ────────┤ VIN               3V3  ├──────── +3V3  (LDO onboard, cấp cho cảm biến)
     GND ────────┤ GND                    │
                 │                 GPIO34 ├──────── SOIL_AO   (ADC1_CH6)
                 │                 GPIO27 ├──────── DHT_DATA  (RMT RX + open-drain)
                 │                 GPIO26 ├──────── RELAY_IN  (kích LOW)
                 │                  GPIO2 ├── LED onboard (trạng thái)
                 │                    USB ├── nạp code / console Serial
                 └────────────────────────┘
```

### 2.3 Khối cảm biến

```
 U2 Cảm biến độ ẩm đất điện dung           U3 DHT22 / AM2302
 ┌──────────┐                              ┌──────────┐        +3V3
 │ VCC      ├── +3V3                       │ VCC (1)  ├── +3V3    │
 │ GND      ├── GND                        │ DATA (2) ├──────┬────┴─ R1 10k (module 3 chân có sẵn)
 │ AOUT     ├──┬── SOIL_AO                 │ NC   (3) │      └── DHT_DATA
 └──────────┘  │                           │ GND  (4) ├── GND
          C4  ═╪═ 100nF                    └──────────┘
               │
              GND
```

### 2.4 Khối chấp hành — relay + bơm

```
 K1 Relay module 1 kênh 5V (opto, kích LOW)
 ┌──────────────┐
 │ IN           ├── RELAY_IN
 │ VCC          ├── +5V
 │ GND          ├── GND
 │              │
 │ COM          ├── +5V
 │ NO           ├──────────────┬────────────┬────────────┐  PUMP+
 │ NC           ├── (bỏ trống) │            │            │
 └──────────────┘          D1 ─┴─ 1N5819   ═╪═ C2       ( M ) M1 bơm 5V
                               ─▲─ K lên    │  100nF     │
                                │  PUMP+    │            │
                               GND ─────────┴────────────┘
```

Relay **đóng** (IN = LOW) thì `COM`–`NO` thông, +5V cấp vào bơm. D1 mắc ngược song song với bơm để dập xung điện áp khi ngắt.

## 3. Netlist

| Net | Kết nối |
|---|---|
| **+5V** | Adapter (+), ESP32 `VIN`, Relay `VCC`, Relay `COM`, C1 (+), C3 (+) |
| **+3V3** | ESP32 `3V3`, Soil `VCC`, DHT22 `VCC`, R1 |
| **GND** | Adapter (−), ESP32 `GND`, Soil `GND`, DHT22 `GND`, Relay `GND`, Bơm (−), D1 anode, C1–C4 (−) |
| **SOIL_AO** | Soil `AOUT`, ESP32 `GPIO34`, C4 |
| **DHT_DATA** | DHT22 `DATA`, ESP32 `GPIO27`, R1 |
| **RELAY_IN** | Relay `IN`, ESP32 `GPIO26` |
| **PUMP+** | Relay `NO`, Bơm (+), D1 cathode, C2 |

## 4. Danh sách linh kiện mỗi nút

| Ký hiệu | Linh kiện | Giá trị / mã | Vai trò |
|---|---|---|---|
| U1 | ESP32 DevKit V1 | ESP32-WROOM-32 | Vi điều khiển, ESP-NOW |
| U2 | Cảm biến độ ẩm đất điện dung | v1.2 / v2.0 | Đo độ ẩm đất (analog) |
| U3 | DHT22 / AM2302 | Module 3 chân hoặc cảm biến 4 chân | Nhiệt độ, độ ẩm không khí |
| R1 | Điện trở kéo lên | 10 kΩ (4.7–10 kΩ) | Kéo DATA lên 3.3V (module 3 chân đã có sẵn) |
| K1 | Module relay 1 kênh | 5V, opto, kích LOW | Đóng/ngắt bơm |
| M1 | Bơm chìm mini | 5V DC, ~100–200 mA | Tưới |
| D1 | Diode Schottky | 1N5819 (hoặc 1N4007) | Dập điện áp ngược của động cơ |
| C1 | Tụ hóa | 1000 µF / 10 V | Ổn định 5V khi bơm khởi động |
| C2 | Tụ gốm | 100 nF | Lọc nhiễu chổi than của bơm |
| C3 | Tụ hóa + gốm | 10 µF + 100 nF | Lọc nguồn sát chân VIN ESP32 |
| C4 | Tụ gốm | 100 nF | Lọc nhiễu ADC tại GPIO34 |
| J1 | Jack DC / terminal | 5.5×2.1 mm | Ngõ vào adapter 5V/2A |

## 5. Lý do thiết kế và lưu ý

### 5.1 Cảm biến độ ẩm đất → GPIO34 (ADC1)

- ADC2 không dùng được khi Wi-Fi/ESP-NOW đang chạy, nên **bắt buộc dùng ADC1** (GPIO32–39).
- GPIO34 chỉ nhận đầu vào, không có pull-up/pull-down, phù hợp cho tín hiệu analog.
- Cấp **3.3V** cho cảm biến để AOUT không vượt dải ADC. Attenuation 11 dB → dải đo ≈ 0–3.1 V.
- Đặc tính cảm biến: **đất khô → ADC cao, đất ướt → ADC thấp**. Firmware quy đổi ra % bằng hai mốc hiệu chuẩn (§6).

### 5.2 DHT22 → GPIO27

- DHT22 dùng giao thức 1 dây riêng: bit 0/1 phân biệt bằng độ dài xung HIGH (~27 µs / ~70 µs). Nếu tự canh thời gian trong code, ngắt Wi-Fi/ESP-NOW sẽ làm đọc sai. Firmware đọc bằng **RMT RX**: phần cứng tự đo độ rộng từng xung với độ phân giải 1 µs.
- Cùng một chân vừa phát tín hiệu bắt đầu (open-drain, kéo LOW 3 ms) vừa để RMT nhận. GPIO27 không phải chân strapping, không thuộc ADC1, nên không ảnh hưởng khối khác.
- Cần điện trở kéo lên 4.7–10 kΩ lên **3.3V** (module 3 chân đã có sẵn). Cấp 3.3V để mức logic DATA không vượt mức chịu đựng của ESP32.
- Khoảng cách giữa 2 lần đọc phải ≥ 2 s (firmware đọc mỗi 5 s); lần đọc đầu tiên sau khi cấp nguồn 2 s.
- Đặt cảm biến ở chỗ thoáng, tránh nước tưới bắn vào: DHT22 dễ trôi số khi ở lâu trong độ ẩm cao. Dây DATA nên dưới 1 m (dài hơn thì dùng điện trở 4.7 kΩ).

### 5.3 Relay → GPIO26

- Tránh các chân strapping (GPIO0, 2, 5, 12, 15) và các chân có xung lúc khởi động, để bơm không giật lúc cấp nguồn.
- Firmware đặt `GPIO26 = HIGH` (relay OFF) trước khi cấu hình output, và pumpTask ép relay OFF mỗi 100 ms khi không tưới.
- **Lưu ý mức logic**: module relay 5V kích LOW có opto với anode LED nối 5V. Khi ESP32 xuất HIGH = 3.3V, LED opto còn chênh khoảng 1.7 V, nên có module **không nhả hẳn**. Khi lắp thử cần kiểm tra:
  1. Nếu relay nhả bình thường ở HIGH thì giữ nguyên.
  2. Nếu không nhả: dùng module có jumper `JD-VCC` (cấp `VCC` opto = 3.3V, `JD-VCC` = 5V), hoặc thay bằng module relay kích HIGH có transistor, hoặc MOSFET logic-level (AO3400/IRLZ44N) + diode D1.

### 5.4 Khối nguồn

- Một adapter 5V/2A cấp cho cả ESP32 (qua `VIN` → LDO AMS1117 3.3V onboard) và bơm. Dòng ước tính: ESP32 ≈ 150–250 mA (đỉnh khi phát), bơm ≈ 100–200 mA (khởi động gấp 2–3 lần), relay ≈ 70 mA.
- **C1 1000 µF** đặt gần relay/bơm để chống sụt áp lúc bơm khởi động, tránh ESP32 bị brownout reset.
- **D1** mắc ngược song song với bơm (cathode về PUMP+) để dập điện áp cảm ứng khi ngắt, bảo vệ tiếp điểm relay. **C2** lọc nhiễu chổi than.
- Đi dây bơm (dòng lớn) tách xa dây AOUT. Nối GND theo hình sao về jack nguồn.
- Khi nạp code qua USB trong lúc vẫn cắm adapter: DevKit V1 có diode trên đường USB 5V nên thường an toàn, nhưng tốt nhất **rút adapter khi nạp** để bơm không chạy ngoài ý muốn.

## 6. Quy trình hiệu chuẩn (console Serial 115200)

1. Trong PowerShell: `. firmware\idf_env.ps1`, `cd firmware\node`, `idf.py -p COMx flash monitor` (thoát monitor: `Ctrl+]`).
2. Gõ `zone N` (1..3) để đặt Zone cho nút. Gõ `mac`, chép dòng `app_config.h: {...}` vào `firmware/components/common/include/app_config.h`.
3. Gõ `stream on` để theo dõi `raw`.
4. **Mốc khô**: lau sạch cảm biến, để ngoài không khí, chờ `raw` ổn định (~20 s), gõ `cal dry`.
5. **Mốc ướt**: nhúng cảm biến vào cốc nước **đến vạch giới hạn** (không ngập phần mạch), chờ ổn định, gõ `cal wet`.
6. `cal show` để kiểm tra. Kết quả lưu trong NVS, mất điện không mất.
7. Cắm vào đất, ghi lại giá trị khi đất khô hẳn và ngay sau khi tưới đẫm để tham khảo khi đặt ngưỡng.
8. Thử bơm: `water 5` → nghe relay đóng, bơm chạy 5 s và tự tắt. Sau đó `water 5` lần nữa sẽ bị `REJ_COOLDOWN` (dùng `cooldown_clear` khi thử).
9. Đo lưu lượng bơm: `water 10` vào cốc đong, rồi tính ml/s (`pump_flow_ml_s`).

## 7. Kiểm thử ESP-NOW (tuần 11)

Chuẩn bị: nạp firmware cho 3 nút và gateway, đọc MAC (`mac` trên console nút, dòng `hello` của gateway), điền vào `firmware/components/common/include/app_config.h`, rồi **build và nạp lại cả 4 board**. Mỗi nút gõ `zone N` một lần (lưu NVS).

Chạy server giả: `pip install pyserial`, sau đó `python server/tools/gw_test.py COM5` (COM của gateway).

| # | Bước | Kết quả mong đợi |
|---|---|---|
| 1 | Cấp nguồn 3 nút | Mỗi nút có `{"t":"node","zone":N,"online":true}`, sau đó `tel` mỗi 30 s (3 nút lệch pha nhau 1 s) |
| 2 | `water 1 5` | `ack` `ok` → `pump on=1` → 5 s sau `pump on=0 dur=5` |
| 3 | `water 1 5` lần nữa | `ack` `rej_cooldown` |
| 4 | `water 2 99` | `ack` `rej_invalid` (vượt `pump_max_s`) |
| 5 | Rút nguồn Zone 3, chờ 90 s | `{"t":"node","zone":3,"online":false}` |
| 6 | `water 3 5` khi Zone 3 đang tắt | `ack` `timeout` (sau 3 × 300 ms) |
| 7 | `cfg 1 40 25 8` | `ack` `ok`; trên console nút, `status` hiện `low=40.0% max=25s fb=8s` |
| 8 | Thoát `gw_test.py` (server "chết"), chờ > 3 phút + 5 phút | LED gateway tắt; trên console nút: `mode -> FALLBACK`; `tel` có `"mode":"fallback"`; đất khô hơn `low` thì nút tự tưới (`src=fallback`) |
| 9 | Chạy lại `gw_test.py` | Trong ≤ 60 s nút về `mode -> NORMAL` |
| 10 | Tầm truyền: đặt nút ở 5/10/20/30 m | Ghi `rssi` và tỉ lệ mất gói (đếm `seq` bị nhảy trong `tel`) |

## 8. Gateway — màn hình TFT SPI 240x320

Gateway hiển thị trạng thái server, giờ, 3 zone (online, độ ẩm đất, nhiệt độ, độ ẩm không khí, bơm, chế độ, RSSI, mất gói) và kết quả lệnh gần nhất. Code: `firmware/gateway/main/lcd.c`, `display.c`; chân và chip cấu hình trong `app_config.h` (`LCD_*`).

| Chân màn hình | Tên khác trên module | ESP32 gateway |
|---|---|---|
| VCC | | 3V3 |
| GND | | GND |
| CS | | GPIO22 |
| RESET | RST | GPIO4 |
| DC | RS, A0 | GPIO21 |
| SDI (MOSI) | SDA, DIN | GPIO23 |
| SCK | CLK, SCL | GPIO18 |
| LED | BL | GPIO16 (hoặc nối thẳng 3V3) |
| SDO (MISO) | | Để trống |
| T_CLK, T_CS, T_DIN, T_DO, T_IRQ | Cảm ứng | Để trống |

- Mặc định chip **ILI9341**. Màn dùng **ST7789** thì đặt `LCD_CONTROLLER LCD_CTRL_ST7789`.
- Đỏ/xanh dương bị đảo: `LCD_SWAP_RB 1`. Chữ bị lật như soi gương: `LCD_MIRROR 1`. Hình lộn ngược: `LCD_FLIP 1`.
- Module có jumper `J1` (bỏ qua LDO) thì để hở khi cấp 3V3 vào chân VCC.
