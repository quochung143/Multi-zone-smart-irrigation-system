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
                            │ Cảm biến độ ẩm đất  │          │ SHT30 (I2C 0x44)    │
                            │ điện dung → GPIO34  │          │ SDA 21 / SCL 22     │
                            └────────────────────┘          └────────────────────┘
```

## 2. Sơ đồ nguyên lý

Vẽ theo từng khối, các khối nối với nhau qua **nhãn net** (`+5V`, `+3V3`, `GND`, `SOIL_AO`, `I2C_SDA`, `I2C_SCL`, `RELAY_IN`, `PUMP+`), giống cách đặt net label trong KiCad/EasyEDA.

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
                 │                 GPIO21 ├──────── I2C_SDA
                 │                 GPIO22 ├──────── I2C_SCL
                 │                 GPIO26 ├──────── RELAY_IN  (kích LOW)
                 │                  GPIO2 ├── LED onboard (trạng thái)
                 │                    USB ├── nạp code / console Serial
                 └────────────────────────┘
```

### 2.3 Khối cảm biến

```
 U2 Cảm biến độ ẩm đất điện dung           U3 SHT30 module
 ┌──────────┐                              ┌──────────┐
 │ VCC      ├── +3V3                       │ VIN      ├── +3V3
 │ GND      ├── GND                        │ GND      ├── GND
 │ AOUT     ├──┬── SOIL_AO                 │ SDA      ├── I2C_SDA   (pull-up 10k có sẵn)
 └──────────┘  │                           │ SCL      ├── I2C_SCL
          C4  ═╪═ 100nF                    │ ADDR     ├── GND       (địa chỉ 0x44)
               │                           └──────────┘
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
| **+3V3** | ESP32 `3V3`, Soil `VCC`, SHT30 `VIN` |
| **GND** | Adapter (−), ESP32 `GND`, Soil `GND`, SHT30 `GND`, SHT30 `ADDR`, Relay `GND`, Bơm (−), D1 anode, C1–C4 (−) |
| **SOIL_AO** | Soil `AOUT`, ESP32 `GPIO34`, C4 |
| **I2C_SDA** | SHT30 `SDA`, ESP32 `GPIO21` |
| **I2C_SCL** | SHT30 `SCL`, ESP32 `GPIO22` |
| **RELAY_IN** | Relay `IN`, ESP32 `GPIO26` |
| **PUMP+** | Relay `NO`, Bơm (+), D1 cathode, C2 |

## 4. Danh sách linh kiện mỗi nút

| Ký hiệu | Linh kiện | Giá trị / mã | Vai trò |
|---|---|---|---|
| U1 | ESP32 DevKit V1 | ESP32-WROOM-32 | Vi điều khiển, ESP-NOW |
| U2 | Cảm biến độ ẩm đất điện dung | v1.2 / v2.0 | Đo độ ẩm đất (analog) |
| U3 | Module SHT30 | I2C 0x44 | Nhiệt độ, độ ẩm không khí |
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

### 5.2 SHT30 → I2C GPIO21/22

- Module SHT30 thường có sẵn điện trở kéo lên 10 kΩ. Không cần thêm.
- `ADDR` nối GND → địa chỉ 0x44. Dây I2C nên dài dưới 1 m.

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

1. Nạp firmware `pio run -e node_zN -t upload`, mở `pio device monitor`.
2. Gõ `mac`, ghi MAC vào `firmware/include/config.h`.
3. Gõ `stream on` để theo dõi `raw`.
4. **Mốc khô**: lau sạch cảm biến, để ngoài không khí, chờ `raw` ổn định (~20 s), gõ `cal dry`.
5. **Mốc ướt**: nhúng cảm biến vào cốc nước **đến vạch giới hạn** (không ngập phần mạch), chờ ổn định, gõ `cal wet`.
6. `cal show` để kiểm tra. Kết quả lưu trong NVS, mất điện không mất.
7. Cắm vào đất, ghi lại giá trị khi đất khô hẳn và ngay sau khi tưới đẫm để tham khảo khi đặt ngưỡng.
8. Thử bơm: `water 5` → nghe relay đóng, bơm chạy 5 s và tự tắt. Sau đó `water 5` lần nữa sẽ bị `REJ_COOLDOWN` (dùng `cooldown clear` khi thử).
9. Đo lưu lượng bơm: `water 10` vào cốc đong, rồi tính ml/s (`pump_flow_ml_s`).

## 7. Kiểm thử ESP-NOW (tuần 11)

Chuẩn bị: nạp firmware cho 3 nút và gateway, đọc MAC (`mac` trên console nút, dòng `hello` của gateway), điền vào `firmware/include/config.h`, rồi **nạp lại cả 4 board**.

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
