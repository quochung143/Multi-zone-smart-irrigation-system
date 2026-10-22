# Kiểm thử phần cứng — Tuần 10 và 11

Kịch bản kiểm thử firmware nút và gateway trên mạch thật. Mỗi bước có kết quả mong đợi; điền cột **Kết quả** (Đạt / Không đạt + ghi chú) khi làm. Số liệu đo được dùng lại cho báo cáo và tuần 16.

Chuẩn bị:

```powershell
. firmware\idf_env.ps1
cd firmware\node;    idf.py -p COMx build flash monitor     # mỗi nút
cd ..\gateway;       idf.py -p COMy build flash             # gateway (không mở monitor, cổng dành cho server)
pip install pyserial                                         # cho server/tools/gw_test.py
```

## Tuần 10 — Firmware nút, kiểm thử từng nút

Làm lần lượt cho cả 3 nút trên breadboard. Lệnh console xem bằng `help`.

### 10.1 Khởi động và định danh

| # | Bước | Mong đợi | Kết quả N1 | N2 | N3 |
|---|---|---|---|---|---|
| 1 | Cấp nguồn, quan sát relay | Relay **không** đóng lúc boot (không giật bơm) | | | |
| 2 | `zone <1-3>` rồi reset (`EN`) | `status` báo đúng Zone sau reset (lưu NVS) | | | |
| 3 | `mac` | In MAC; ghi vào bảng 10.5 | | | |

### 10.2 Độ ẩm đất: lọc và hiệu chuẩn

Theo quy trình [HARDWARE.md §6](HARDWARE.md#6-quy-trình-hiệu-chuẩn-console-serial-115200).

| # | Bước | Mong đợi | Kết quả N1 | N2 | N3 |
|---|---|---|---|---|---|
| 1 | `stream on`, để cảm biến yên 1 phút | `raw` dao động ≤ ±15 (nhờ oversample 16 + median 5 + EMA 0.2) | | | |
| 2 | Cảm biến ngoài không khí → `cal dry` | `Da luu moc KHO`; ghi giá trị dry | | | |
| 3 | Nhúng nước tới vạch → `cal wet` | `Da luu moc UOT`; ghi giá trị wet; dry − wet ≥ 500 | | | |
| 4 | Cắm vào đất khô / vừa / ướt | `soil` lần lượt thấp / giữa / cao, trong 0–100 % | | | |
| 5 | Rút dây AOUT | Trong vài giây `flags` có bit `0x02` (`[ADC ERR]` trong `status`) | | | |
| 6 | Reset nút, `cal` | Giữ nguyên dry/wet (NVS) | | | |

### 10.3 DHT22

| # | Bước | Mong đợi | Kết quả N1 | N2 | N3 |
|---|---|---|---|---|---|
| 1 | `status` sau 10 s | Nhiệt độ, độ ẩm hợp lý (so với nhiệt kế/điện thoại, sai ≤ 1 °C, ≤ 5 %RH) | | | |
| 2 | Hà hơi vào cảm biến | `hum` tăng rõ trong 5–10 s | | | |
| 3 | Rút dây DATA | `[DHT ERR]`, `flags` có bit `0x01`; cắm lại thì hết lỗi | | | |
| 4 | `stream on` 10 phút khi Wi-Fi/ESP-NOW chạy | Không mất mẫu thường xuyên (RMT không bị ngắt Wi-Fi làm sai) | | | |

### 10.4 Bơm và an toàn

| # | Bước | Mong đợi | Kết quả N1 | N2 | N3 |
|---|---|---|---|---|---|
| 1 | `water 5` | Relay đóng, bơm chạy ~5 s rồi tự tắt; log `bom OFF src=cmd chay=5s` | | | |
| 2 | `water 5` ngay sau đó | `REJ_COOLDOWN` (cooldown 10 phút) | | | |
| 3 | `cooldown_clear`, `water 20`, sau 3 s gõ `water 5` | Lệnh 2 bị `REJ_BUSY` | | | |
| 4 | `stop` khi bơm đang chạy | Bơm tắt ngay | | | |
| 5 | `water 31` | `REJ_INVALID` (vượt `pump_max_s` = 30) | | | |
| 6 | `cooldown_clear`, `water 30` | Chạy đủ 30 s, log `src=cmd` (không phải `safety`) | | | |
| 7 | Mức HIGH của GPIO26 | Relay nhả hẳn (xem HARDWARE.md §5.3 nếu không nhả) | | | |
| 8 | Bơm chạy, đo VIN bằng đồng hồ | Không brownout reset khi bơm khởi động | | | |

### 10.5 Địa chỉ MAC

| Thiết bị | MAC | Dòng điền vào `app_config.h` |
|---|---|---|
| Gateway (dòng `hello`) | | `GATEWAY_MAC` |
| Nút Zone 1 | | `NODE_MAC[0]` |
| Nút Zone 2 | | `NODE_MAC[1]` |
| Nút Zone 3 | | `NODE_MAC[2]` |

Điền xong thì build và nạp lại **cả 4** ESP32.

## Tuần 11 — ESP-NOW, gateway, chế độ an toàn

Chạy công cụ server giả trên cổng gateway:

```powershell
python server\tools\gw_test.py COMy --log tuan11.jsonl
```

Lệnh trong công cụ: `water`, `stop`, `cfg`, `stats`, `selftest <zone>`, `hb on|off`, `quiet on|off`.

### 11.1 Truyền nhận cơ bản

| # | Bước | Mong đợi | Kết quả |
|---|---|---|---|
| 1 | Reset gateway | Dòng `{"t":"hello",...}` | |
| 2 | Cấp nguồn 3 nút | Mỗi nút có dòng `tel` mỗi 30 s, lệch pha nhau ~1 s; `{"t":"node","zone":z,"online":true}` | |
| 3 | So `soil`, `temp`, `hum` trong `tel` với `status` trên console nút | Trùng nhau | |
| 4 | Sau 60 s | Có dòng `stat` cho từng zone | |
| 5 | Console nút: `status` | `mode=NORMAL`, `gateway cuoi` < 60 s | |

### 11.2 Lệnh, ACK, chống trùng

| # | Bước | Mong đợi | Kết quả Z1 | Z2 | Z3 |
|---|---|---|---|---|---|
| 1 | `selftest <z>` (xóa cooldown trên nút trước) | `10/10 PASS` | | | |
| 1b | Ngay sau selftest (đang `NGHI`): `water <z> 3` rồi `water <z> 3 force` | Lệnh 1 `rej_cooldown`; lệnh 2 `ok`, `pump src=force` | | | |
| 2 | `water <z> 5` | `ack ok`, rồi `pump on=1`, sau 5 s `pump on=0 dur=5` | | | |
| 3 | `raw {"t":"cmd","zone":1,"seq":<seq vừa dùng>,"dur":5}` (gửi lại seq cũ) | Trả lại ACK cũ, **bơm không chạy lại** | | | |
| 4 | Rút nguồn nút, `water <z> 5` | `ack timeout` sau ~1 s (3 lần × 300 ms) | | | |
| 5 | `raw {"t":"cmd","zone":9,"seq":1,"dur":5}` | Dòng `# lenh khong hop le` | | | |
| 6 | `raw {hong` | Dòng `# json loi` | | | |

### 11.3 Phát hiện nút offline

| # | Bước | Mong đợi | Kết quả |
|---|---|---|---|
| 1 | Rút nguồn 1 nút, bấm giờ | Sau 90–120 s có `{"t":"node","zone":z,"online":false}` | |
| 2 | Cấp nguồn lại | `online:true` ngay khi có `tel` đầu tiên | |

### 11.4 Fallback khi mất server / gateway

Đặt ngưỡng cao để dễ thấy nút tự tưới: `cfg <z> 90 30 5` (ngưỡng 90 %, fallback 5 s) với cảm biến đang ở đất khô.

| # | Bước | Mong đợi | Kết quả |
|---|---|---|---|
| 1 | `hb off`, bấm giờ | Sau ~3 phút LED GPIO2 gateway tắt (ngừng beacon) | |
| 2 | Chờ tiếp | Trong 4–5 phút sau đó (5 phút kể từ beacon cuối) nút log `che do -> FALLBACK`, LED nút sáng liên tục, `tel` có `"mode":"fallback"` | |
| 3 | Đất khô hơn ngưỡng | Nút tự tưới 5 s: `pump src=fallback cmd_seq=0`; lặp lại sau mỗi cooldown 10 phút | |
| 4 | Rút dây cảm biến đất khi đang FALLBACK | Không tự tưới (cảm biến lỗi thì không bơm) | |
| 5 | `hb on` | Beacon tiếp tục; nút về `NORMAL` trong ≤ 60 s | |
| 6 | Rút nguồn gateway hẳn | Nút vào FALLBACK sau 5 phút | |
| 7 | Trả cấu hình: `cfg <z> 35 30 10` | `ack ok` | |

### 11.5 Độ ổn định và tỉ lệ mất gói

Để hệ thống chạy ≥ 1 giờ (3 nút, khoảng cách thực tế của mô hình), sau đó gõ `stats`.

| Zone | Khoảng cách | RSSI TB (dBm) | rx | lost | Tỉ lệ mất (%) | dup | Ghi chú |
|---|---|---|---|---|---|---|---|
| 1 | | | | | | | |
| 2 | | | | | | | |
| 3 | | | | | | | |

Mục tiêu: tỉ lệ mất < 1 %, không có reset ngoài ý muốn (giá trị `up` trong `tel` chỉ tăng). File `tuan11.jsonl` giữ lại để vẽ RSSI/mất gói trong báo cáo.
