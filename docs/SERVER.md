# SERVER — Thiết kế và hiện trạng code (Gateway ⇄ Server qua MQTT)

> Tổng kết buổi chốt thiết kế server ngày 25/10/2026 và phần code đã viết theo thiết kế đó.
> Sơ đồ kiến trúc: `.archify/architecture-irrigation-mqtt-20261025-215108/irrigation-mqtt.html` (mở bằng trình duyệt).
> Tài liệu này **thay thế** các phần Serial trong `DESIGN.md` §2, §6, §7 và `PROTOCOL.md` §5. Hai file đó chưa được sửa (xem §11).

---

## 1. Thay đổi so với DESIGN.md

| Hạng mục | Trước | Nay |
|---|---|---|
| Gateway ⇄ Server | USB-Serial 115200, JSON lines | **Wi-Fi + MQTT** qua broker **Mosquitto** trên laptop. Payload JSON giữ nguyên |
| Mạng | — | **Hotspot điện thoại** (cả lúc thu dữ liệu và lúc demo ở trường) |
| Kênh ESP-NOW | Ghi cứng kênh 1 | Kênh theo hotspot (không chọn được) → **nút tự dò kênh 1–13** |
| Thư viện MongoDB | `motor` | `pymongo.AsyncMongoClient` (motor đã hết hỗ trợ từ 05/2026) |
| `serial_bridge.py` | Thread đọc COM | `mqtt_link.py` (aiomqtt) |
| `server_sessions` | Phiên Serial | `gw_sessions` (gateway online/offline) + `srv_sessions` (server bật/tắt) |
| AI | Tự thu dữ liệu 4 tuần | Tầng 1 train trên **dataset công khai** rồi tinh chỉnh/test bằng 2–3 ngày dữ liệu thật; tầng 2 hiệu chuẩn tại chỗ |
| USB của gateway | Kênh dữ liệu | Chỉ còn log và console cấu hình |

## 2. Kiến trúc

```
Nút Zone 1..3 ──ESP-NOW + ACK──► Gateway ESP32 ──Wi-Fi──► Hotspot điện thoại ──MQTT :1883──► Mosquitto (laptop)
                                                                                                │ sub / pub
 ┌──────────────────────────── Server FastAPI (1 tiến trình, 1 worker) ──────────────────────────▼───────────┐
 │ mqtt_link ──dispatch──► ingest ──► registry (RAM, write-through `zones`)                                 │
 │     ▲                     ├──► events (irrigation_events, soil_after_10m)                                │
 │     │ cmd                 ├──► db (MongoDB)                                                              │
 │ commands ◄── decision ────┘──► ws ──► Web dashboard                                                      │
 │     ▲          └──► ml_runtime (ml/features.py + joblib)                                                 │
 │     └── api (tưới tay / stop / cfg)                                                                      │
 └──────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Nguyên tắc vẫn giữ như cũ: **server là "bộ não"**, nút tự bảo vệ (bơm ≤ 30 s, cooldown 10 phút), và **nút tự tưới fallback khi mất server quá 5 phút**.

## 3. MQTT

### 3.1 Topic

| Hướng | Topic | Payload | QoS | Retain |
|---|---|---|---|---|
| GW→S | `irr/z/{zone}/tel` | `{"t":"tel","zone":1,"seq":120,"soil":43.2,"soil_raw":2345,"temp":28.41,"hum":71.2,"pump":0,"mode":"normal","flags":0,"up":3600,"rssi":-58}` | 0 | – |
| GW→S | `irr/z/{zone}/pump` | `{"t":"pump","zone":2,"on":0,"src":"cmd","cmd_seq":7,"dur":15}` | 1 | – |
| GW→S | `irr/z/{zone}/ack` | `{"t":"ack","zone":2,"seq":7,"status":"ok"}` | 1 | – |
| GW→S | `irr/z/{zone}/node` | `{"t":"node","zone":3,"online":true}` | 1 | ✔ |
| GW→S | `irr/z/{zone}/stat` | `{"t":"stat","zone":1,"rx":120,"lost":2,"dup":1,"bad":0}` | 0 | – |
| GW→S | `irr/gw/status` | `online` / `offline` (chuỗi, **LWT**) | 1 | ✔ |
| S→GW | `irr/z/{zone}/cmd` | `{"t":"cmd","zone":2,"seq":7,"dur":15[,"force":1]}`, `{"t":"stop",...}`, `{"t":"cfg","zone":1,"seq":9,"low":35.0,"max":30,"fb":10}` | 1 | – |
| S→GW | `irr/srv/time` | `{"t":"time","epoch":1794000000}`, gửi mỗi 60 s | 0 | – |
| S→GW | `irr/srv/status` | `online` / `offline` (chuỗi, **LWT**) | 1 | ✔ |

- QoS 1 có thể giao một lệnh hai lần. Điều này vô hại vì nút bỏ qua lệnh trùng `seq` và trả lại ACK cũ.
- Server **bỏ qua `node=online` đến từ bản tin retain**, vì đó có thể là trạng thái cũ của phiên trước. Server chỉ coi nút là online khi nhận được gói thật từ nút.

### 3.2 Server còn sống (phía gateway)

`server_alive()` = `irr/srv/status == online` **và** đã nhận `irr/srv/time` trong vòng 3 phút. Nếu không thỏa (kể cả khi mất kết nối với broker), gateway ngừng phát beacon, và sau 5 phút các nút vào FALLBACK. Hành vi này giống hệt bản Serial cũ.

### 3.3 Bảo mật broker

- `allow_anonymous false`, dùng 2 tài khoản `gateway` và `server`. ACL nằm trong `server/mosquitto/acl`: gateway chỉ được publish chiều lên, chỉ được đọc `cmd` và `irr/srv/#`.
- Đã thử trên broker thật: client ẩn danh bị từ chối kết nối, tài khoản `gateway` publish vào `irr/z/1/cmd` bị chặn (`Denied PUBLISH`).

## 4. Module server (`server/app/`)

| File | Vai trò |
|---|---|
| `config.py` | Đọc `.env` (pydantic-settings). Chứa các hằng số: chu kỳ quyết định 5 phút, dữ liệu cũ 120 s, chờ ACK 5 s, `time` 60 s |
| `mqtt_link.py` | Client aiomqtt: LWT, publish `online`, subscribe các topic chiều lên, gửi `time` mỗi 60 s, tự kết nối lại sau 5 s. Tắt có chủ đích thì publish `offline` ngay |
| `ingest.py` | Phân luồng theo topic, gán `ts = now()` (giờ server là chuẩn). Ghi `readings`, cập nhật registry, gọi events/commands, đẩy WS |
| `registry.py` | `ZoneRegistry`: cấu hình (`ZoneConfig`, có kiểm tra ràng buộc) và trạng thái runtime. Gốc dữ liệu là collection `zones`: ghi Mongo trước rồi mới đổi RAM |
| `commands.py` | Cấp `seq` (`$inc` trong `zones.next_seq`, vòng 1..65535, giá trị đầu lấy theo thời gian). Lưu `commands` với trạng thái `pending`, rồi chuyển sang `ok`/`rej_*`/`timeout`/`busy`, hoặc `no_ack` khi quá 5 s hay gateway offline |
| `events.py` | Ghép bản tin pump on/off thành `irrigation_events`, kèm các cờ `end_estimated`/`start_estimated` khi mất gói. Job mỗi phút điền `soil_after_10m` (trung bình [+8, +12] phút); nếu không có dữ liệu thì ghi `after_missing` |
| `decision.py` | Chạy đúng mốc :00/:05, các Zone cách nhau 2 s. Gồm `THRESHOLD` (có `collect_mode` với `dur` ngẫu nhiên 5–25 s), `AI` và `MANUAL`. Mọi quyết định đều ghi vào `predictions.action_taken` |
| `ml_runtime.py` | Nạp `ml/models/meta.json` cùng các file joblib, gọi `ml/features.py` (dùng chung với lúc train) |
| `db.py` | `MongoStore` (MongoDB 7.x, `readings` là time-series) và `MemoryStore` (`MONGO_URI=memory://`), dùng chung một giao diện |
| `api.py`, `ws.py` | REST và WebSocket |
| `main.py`, `__main__.py` | Lifespan: khởi động/dừng task, đóng các phiên còn treo. Chạy bằng `python -m app`, dùng SelectorEventLoop trên Windows |

### 4.1 Quyết định mỗi 5 phút

Thứ tự kiểm tra cho mỗi Zone: `MANUAL` → `skip_manual`; nút offline → `skip_offline`; dữ liệu cũ hơn 2 phút → `skip_stale`; cờ lỗi cảm biến đất (flags bit1) → `skip_sensor`. Sau đó tính `need`:

- **THRESHOLD**: `soil < soil_low`. `dur` = `threshold_dur`, hoặc ngẫu nhiên 5–25 s khi `collect_mode`.
- **AI**: chưa có model → `skip_no_model`. Ngược lại `p_need = clf(x)`, `need = p_need ≥ 0.5`, và `dur = clip(reg(x), 3, pump_max_s)`. Nếu không có `reg` thì dùng `threshold_dur`.

Nếu `need` đúng: bơm đang chạy → `skip_pump_on`; còn cooldown (tính từ `end_ts` của lần tưới gần nhất, bất kể nguồn) → `skip_cooldown`; gateway offline → `skip_gw_offline`; còn lại gửi lệnh với `action_taken = water`.

### 4.2 Kết nối lại (resync)

- Server khởi động: publish `irr/srv/status=online` rồi `time`.
- Nhận `irr/gw/status=online`: gửi `time`, sau đó `cfg` cho cả 3 Zone.
- Một Zone chuyển offline → online: gửi `cfg` cho Zone đó.
- Gateway offline: mọi lệnh `pending` chuyển thành `no_ack`, các Zone bị đánh dấu offline, phiên `gw_sessions` được đóng.
- Không đệm dữ liệu khi mất kết nối. Các khoảng trống được suy ra từ `gw_sessions` và `srv_sessions` khi làm sạch dữ liệu train.

## 5. Dữ liệu (MongoDB)

Giữ các collection như DESIGN §8, với các thay đổi sau:

- `zones` thêm `fallback_s` (thời lượng nút tự tưới khi FALLBACK, ≤ `pump_max_s`) và `next_seq`.
- `commands` thêm `force` và trạng thái `no_ack`.
- `irrigation_events.source` ∈ `cmd`/`manual`/`fallback`; `decided_by` ∈ `threshold`/`ai`/`manual`/`node`/`unknown`.
- `predictions` thêm `dur` (thời lượng thực gửi đi) và `cmd_seq`.
- Collection mới: `link_stats` (`ts, zone_id, rx, lost, dup, bad`), `gw_sessions`, `srv_sessions`.

## 6. REST API và WebSocket

| Method | Endpoint | Ghi chú |
|---|---|---|
| GET | `/api/health` | `{mqtt, gateway, model, auth}` |
| GET | `/api/zones` | Snapshot: cấu hình + trạng thái 3 Zone + gateway + model |
| PUT | `/api/zones/{id}` | Cập nhật một phần. Sai ràng buộc → **422**. Chuyển sang `AI` khi chưa có model → **409**. Tự gửi `cfg` nếu gateway online |
| POST | `/api/zones/{id}/water` | `{"dur": N}` với N ∈ [1, `pump_max_s`]. Luôn gửi `force=1` (bỏ qua cooldown). Zone hoặc gateway offline → **409** |
| POST | `/api/zones/{id}/stop` | Gateway offline → 409 |
| GET | `/api/readings?zone=&from=&to=&bucket=` | Tự chọn bucket: ≤ 6 giờ lấy dữ liệu thô, ≤ 24 giờ gom 5 phút, dài hơn gom 30 phút. `bucket=0` để ép lấy dữ liệu thô |
| GET | `/api/events`, `/api/predictions` | Mặc định 24 giờ gần nhất |
| GET | `/api/commands?limit=50` | |
| GET | `/api/stats?zone=&from=&to=` | §9.5: số lần tưới, tổng giây, ml (cần `pump_flow_ml_s`), độ lệch chuẩn độ ẩm, % thời gian trong [`soil_low`, 75] |

Ràng buộc cấu hình: `soil_low` ∈ [10, 80], `soil_target` ≥ `soil_low` + 5, `pump_max_s` ∈ [1, 30], `threshold_dur` ∈ [3, `pump_max_s`], `fallback_s` ≤ `pump_max_s`, `cooldown_s` ≥ 0.

**WebSocket `/ws`**: mỗi bản tin có dạng `{"type", "zone", "ts", "data"}` với `type` ∈ `snapshot` (gửi ngay khi kết nối), `tel`, `pump`, `ack`, `command`, `node`, `gateway`, `prediction`, `zone`, `stat`.

## 7. Dashboard

Trang `http://<laptop>:8000/` gồm:

- **Thẻ Zone**: độ ẩm, nhiệt độ, độ ẩm không khí, bơm, chế độ của nút, RSSI, thời điểm thấy lần cuối, quyết định gần nhất. Mỗi thẻ có chọn mode, nút Tưới N giây / Dừng, và form cấu hình.
- **Biểu đồ**: độ ẩm đất 6 giờ / 24 giờ / 7 ngày (có đánh dấu các lần tưới), và nhiệt độ/độ ẩm không khí theo Zone.
- **Nhật ký**: lệnh kèm trạng thái ACK, sự kiện bơm, nút và gateway.

Chart.js được lưu sẵn trong `static/vendor/` nên dashboard không cần Internet.

## 8. Cách chạy

### 8.0 Chạy bằng Docker (cách chính)

`docker-compose.yml` ở thư mục gốc repo chạy cả 3 thành phần phía laptop:

| Container | Image | Cổng | Ghi chú |
|---|---|---|---|
| `mosquitto` | `eclipse-mosquitto:2.0.22` | `1883` (mọi mạng) | Gateway nối vào qua hotspot. Dùng `server/mosquitto/mosquitto.docker.conf`, `passwd`, `acl` |
| `mongo` | `mongo:7.0` | `127.0.0.1:27017` (chỉ trong máy) | Dữ liệu lưu ở volume `mongo-data`, không mất khi `down` hay khi cập nhật image |
| `server` | build từ `server/Dockerfile` (Python 3.12) | `8000` | Đọc `server/.env`. Riêng `MQTT_HOST`, `MONGO_URI` bị compose ghi đè để trỏ vào container khác. `ml/models` được mount chỉ đọc. Log ghi ra `server/logs` |

Chạy (PowerShell, tại `D:\project\doan2`):

```powershell
docker compose up -d --build     # lần đầu khoảng vài phút (image server ~1.5 GB vì có pandas/sklearn/xgboost)
docker compose ps                # cả 3 phải "Up", riêng mongo phải "healthy"
docker compose logs -f server    # xem log server
docker compose down              # dừng; thêm -v nếu muốn XOÁ HẲN dữ liệu MongoDB
```

- **Cần chuẩn bị trước**: `server/.env` (§8.1, nhớ `DASH_PASS`) và `server/mosquitto/passwd`. File passwd tạo bằng `mosquitto_passwd` của bản Windows vẫn dùng được trong container.
- **Không chạy song song với Mosquitto trên Windows**, vì cả hai cùng chiếm cổng 1883. Hãy đóng cửa sổ `mosquitto.exe`. Service `mosquitto` đã được chuyển sang Manual. Rule tường lửa cho cổng 1883 vẫn giữ nguyên.
- **Tự chạy lại**: các container có `restart: unless-stopped`, nên tự chạy lại khi Docker Desktop khởi động. Bật Settings → General → *Start Docker Desktop when you sign in* để laptop vừa mở là hệ thống chạy.
- **Truy cập từ xa**: `cloudflared` vẫn là service Windows trỏ tới `http://127.0.0.1:8000` (§13), không phải sửa gì.
- **Cập nhật code**: sau khi sửa code server, chạy lại `docker compose up -d --build`. Có model mới thì chép vào `ml/models` rồi `docker compose restart server`, không cần build lại.
- **Kiểm tra nhanh** khi đang có gateway hoặc giả lập gửi dữ liệu (tạo phiên từ `DASH_PASS` trong container, tưới thử Zone 1 trong 3 s):
  ```powershell
  Get-Content server\tools\docker_smoke.py -Raw | docker compose exec -T server python -
  ```
- `MONGO_URI=memory://` chỉ còn dùng khi chạy server trực tiếp bằng `python -m app` (§8.2).

### 8.1 Cài đặt (một lần)

```powershell
pip install -r server/requirements.txt
# Tài khoản broker (file passwd không commit)
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" -c server\mosquitto\passwd server
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" server\mosquitto\passwd gateway
copy server\.env.example server\.env   # rồi sửa MQTT_PASS
```

Chạy bằng Docker (§8.0) thì MongoDB có sẵn trong container. Nếu chạy trực tiếp (§8.2) mà máy chưa cài MongoDB thì đặt `MONGO_URI=memory://`.

Nếu Mosquitto đã được cài làm Windows service và đang chiếm cổng 1883, hãy dừng service đó, hoặc trỏ service sang dùng `server/mosquitto/mosquitto.conf`.

### 8.2 Chạy trực tiếp không qua Docker (từ thư mục gốc repo)

```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -c server\mosquitto\mosquitto.conf -v   # cửa sổ 1
cd server; python -m app                                                             # cửa sổ 2
python server/tools/gw_bridge.py COM5 --password <mk gateway>                        # cửa sổ 3 (board thật, firmware Serial)
python server/tools/gw_bridge.py sim --password <mk gateway> --tel 5                 # hoặc: giả lập gateway + 3 nút
```

`gw_bridge.py` chỉ dùng cho **firmware gateway Serial cũ (gw-1.0)** hoặc để giả lập. Nó đóng vai gateway MQTT (LWT, retain `node`, chuyển `cmd`/`time` xuống Serial). Với firmware gw-2.0 (§8.3), gateway tự nói MQTT nên không cần cửa sổ 3. Không chạy uvicorn với `--reload` hoặc nhiều worker. Log nằm ở `server/logs/server.log`, xoay vòng hằng ngày và giữ 14 ngày.

### 8.3 Firmware gateway gw-2.0 (MQTT)

Build và nạp từ PowerShell:

```powershell
. firmware\idf_env.ps1
cd firmware\gateway; idf.py -p COM5 flash monitor
```

- **Phân vùng app đã đổi** sang 1.5 MB (`SINGLE_APP_LARGE`), vì Wi-Fi + MQTT + mDNS + TFT vượt 1 MB. `idf.py flash` sẽ ghi cả bảng phân vùng mới.
- Lần build đầu sẽ tải component `espressif/mdns` về `managed_components/`.

Cổng USB giờ là **console `gw>`** (không còn là kênh JSON). Cấu hình một lần, lưu NVS, rồi `restart`:

```
gw> wifi "Ten Hotspot" matkhau          # ssid có dấu cách thì đặt trong ngoặc kép
gw> broker TEN-LAPTOP.local             # tên máy Windows + .local (mDNS), hoặc IP; thêm port nếu khác 1883
gw> broker_ip 192.168.43.20             # IP dự phòng khi mDNS không tìm thấy ('-' để xoá)
gw> mqtt_user gateway <mat khau>        # tài khoản trong server/mosquitto/passwd
gw> restart
gw> status                              # Wi-Fi/IP/kênh, broker, MQTT, server, 3 zone
gw> net                                 # xem cấu hình đã lưu (mật khẩu được ẩn)
```

Hành vi:
- **Có Wi-Fi**: ESP-NOW chạy theo kênh của hotspot, vì peer khai báo kênh 0 và tắt modem sleep để không rớt gói.
- **Chưa cấu hình Wi-Fi**: ESP-NOW đứng yên ở `ESPNOW_CHANNEL` = 1 như bản cũ.
- **Mất kết nối và phân giải lại**: mất Wi-Fi thì thử lại sau 5 s. Nối broker lỗi 3 lần liên tiếp thì phân giải lại tên laptop, phòng khi IP đổi theo DHCP của hotspot.
- **Nối broker lần đầu sau khi khởi động**: gateway publish `offline` rồi `online` lên `irr/gw/status`, để server chắc chắn gửi lại `time` và `cfg`.
- **Khi server sống lại**: gateway publish lại `node` của mọi zone đã điền MAC, nhờ đó xoá các trạng thái retain cũ.
- **Màn hình TFT**: góc phải hiển thị `MQTT OK/--` (hoặc `WIFI --`) và `SERVER OK/--`. Dòng thứ hai hiển thị giờ, SSID, IP gateway và kênh `cN`.

Nút phải chạy firmware có dò kênh (§8.4) thì mới theo được kênh của hotspot.

### 8.4 Firmware nút: dò kênh ESP-NOW

Gateway chạy theo kênh của hotspot điện thoại. Kênh này không chọn được và có thể đổi mỗi lần bật hotspot. Vì vậy nút tự tìm kênh của gateway ([firmware/node/main/comm.c](../firmware/node/main/comm.c)):

- **Khi nào dò**:
  - khi gửi ESP-NOW lỗi ở lớp MAC **2 lần liên tiếp** (`SCAN_AFTER_FAILS`; mỗi lần đã gồm 2 lần gửi lại);
  - khi **gói đầu tiên sau khởi động** gửi lỗi;
  - khi gõ `scan` trên console nút.
- **Cách dò**: nút gửi lại **chính gói đó, giữ nguyên `seq`**, lần lượt trên các kênh 1–13 (tối đa 2 vòng, mỗi kênh chờ 100 ms, tổng cộng khoảng 2,6 s), bắt đầu từ kênh hiện tại. Kênh nào gateway trả ACK lớp MAC thì giữ kênh đó và **lưu vào NVS** (khoá `ch`). Lần khởi động sau, nút bắt đầu thẳng từ kênh này.
- **Dò không thấy** (gateway tắt, hoặc đang nối lại Wi-Fi): nút quay về kênh cũ và đợi 60 s (`SCAN_RETRY_MS`) mới dò lại. Bộ đếm FALLBACK 5 phút và bơm vẫn chạy bình thường. Mỗi lần dò chỉ làm chậm task comm khoảng 2,6 s.
- **Vì sao không dò theo mất beacon** (khác với quyết định B1 ban đầu): gateway **cố ý ngừng beacon khi server chết** để nút vào FALLBACK. Lúc đó gateway vẫn ở đúng kênh và vẫn trả ACK lớp MAC. Nếu dò theo beacon, mỗi lần tắt laptop cả 3 nút sẽ dò liên tục mà không có lý do.
- **Peer ESP-NOW khai báo kênh 0** (= kênh hiện tại của radio) ở cả nút lẫn gateway, nên khi đổi kênh không phải khai báo lại peer.
- **Console nút**: `status` hiển thị thêm `kenh N`; lệnh mới `scan` để dò lại ngay.

## 9. Kiểm thử

- `cd server; python -m pytest -q`: **24 test đạt**, không cần broker hay MongoDB (dùng `MemoryStore` cùng link giả). Các test bao phủ:
  - resync khi gateway online;
  - ghi reading và trạng thái online;
  - bỏ các bản tin lỗi;
  - ACK, `no_ack`, gateway offline;
  - seq quay vòng;
  - ghép pump on/off, mất gói off/on, `soil_after_10m`;
  - mọi nhánh của THRESHOLD và AI, `collect_mode`;
  - ràng buộc cấu hình;
  - API (422/409/404, force, bucket) và snapshot WS;
  - `build_features`;
  - bỏ qua retain `node`.
- **Chạy đầu cuối thật** (Mosquitto 2.0.22 có xác thực + ACL, server `memory://`, `gw_bridge.py sim` với Zone 3 giả lập offline):
  - Gateway online thì server gửi `time` và `cfg` cho 3 Zone. Zone 1 và 2 trả `ok`, Zone 3 trả `timeout`.
  - Tưới tay Zone 1 trong 4 s: nhận ACK `ok`, event được ghi với `duration_s=4` và `decided_by=manual`.
  - Đến mốc 5 phút, Zone 2 (THRESHOLD, `soil_low=70`) tự tưới 6 s và event ghi `decided_by=threshold`. Zone 1 ở MANUAL trả `skip_manual`, Zone 3 trả `skip_offline`.
  - Cấu hình sai bị trả 422. Client ẩn danh bị broker từ chối, và tài khoản gateway không publish được vào topic `cmd`.
- **Chạy bằng Docker trên MongoDB thật** (26/10/2026, project `irrtest` dùng cổng riêng, `gw_bridge.py sim` gửi dữ liệu):
  - `MongoStore` khởi tạo được time-series collection và các index.
  - Server đăng nhập broker trong container. API trả 401 khi chưa đăng nhập.
  - Tưới tay Zone 1 trong 3 s: ACK `ok`, event được ghi `decided_by=manual`, `duration_s=3`.
  - Đọc readings thô và gom 5 phút (`$dateTrunc`) đều đúng. `commands` và `/api/stats` trả đúng.
- **Chạy trên board thật** (26/10/2026, hotspot `piwifi` kênh 1, gateway gw-2.0, nút Zone 1, sau đó chuyển sang `docker compose`):
  - Gateway vào hotspot và tìm thấy laptop qua mDNS (`DESKTOP-2NGE9T0.local`). Nó kết nối được broker sau khi mở cổng 1883 trên tường lửa Windows, cả với Mosquitto chạy trên Windows lẫn trong Docker.
  - Server chạy trong Docker với MongoDB thật. Dashboard truy cập được qua `https://api.quochung.xyz`, có đăng nhập.
  - Tưới tay và dừng từ dashboard: ACK `ok`. Bấm tưới khi bơm đang chạy thì nhận `rej_busy`, đúng thiết kế. Lịch sử tưới ghi `decided_by=manual`.
  - Khi đổi broker (từ Windows sang Docker), gateway mất khoảng 1 phút mới kết nối lại: thử 3 lần rồi mới phân giải lại tên laptop.
- **Chưa kiểm thử**:
  - chế độ AI với model thật (chưa có model);
  - board thật đi qua `gw_bridge.py`.

## 10. Lộ trình thực hiện

| Giai đoạn | Nội dung | Trạng thái |
|---|---|---|
| P1 (tuần 12) | `mqtt_link`, `ingest`, `db`, `commands`, `events`, `decision` (THRESHOLD/MANUAL), REST, WS | **Đã code + test** |
| P2 (tuần 13) | Dashboard, API lịch sử | **Đã code** (chưa thử trên trình duyệt) |
| P3 (tuần 15) | `ml_runtime`, `AIPolicy`, `/api/stats` | **Đã code phần server**. Còn thiếu model: `ml/label.py`, `ml/train.py` và adapter dataset công khai vẫn trống |

## 11. Việc còn lại

1. ~~Firmware gateway → MQTT~~: **đã code (gw-2.0, §8.3)**, build sạch không cảnh báo trên ESP-IDF v5.5.5. **Chưa nạp và thử trên board.** Cần kiểm tra:
   - nối được hotspot và broker, cả khi dùng `.local` lẫn IP dự phòng;
   - lệnh tưới từ dashboard đi tới nút và ACK quay về;
   - tắt server thì gateway ngừng beacon, và sau 5 phút nút vào FALLBACK;
   - tắt hotspot thì gateway tự nối lại.
2. ~~Firmware nút: dò kênh 1–13~~: **đã code (§8.4)**, build sạch. **Chưa nạp và thử trên board.** Cần kiểm tra:
   - đổi kênh hotspot (tắt rồi bật lại, hoặc dùng điện thoại khác): nút tự tìm lại gateway trong vòng khoảng 1 phút;
   - rút nguồn gateway: nút dò mỗi 60 s, vẫn vào FALLBACK sau 5 phút và vẫn tưới fallback;
   - tắt server mà vẫn để gateway chạy: nút **không** dò kênh, nhưng vẫn vào FALLBACK.
3. **Kiểm tra sớm** xem hotspot điện thoại có bật client isolation không, tức các thiết bị trong cùng hotspot có thấy nhau không.
4. ~~Cài MongoDB 7.x~~: đã chạy bằng Docker (§8.0). Còn lại: chuyển hệ thống đang chạy thật sang `docker compose` và chạy liên tục ≥ 24 giờ.
5. Cập nhật `DESIGN.md` (§2, §6, §7, §8, §11, §14, §15) và `PROTOCOL.md` §5 theo tài liệu này.
6. AI:
   - Kiểm tra dataset Mendeley [jtsyb593fy](https://data.mendeley.com/datasets/jtsyb593fy/1) (chính) và [fpdwmm7nrb](https://data.mendeley.com/datasets/fpdwmm7nrb/1) (phụ): cột dữ liệu, chu kỳ đo, cách sinh nhãn.
   - Viết adapter dataset → DataFrame chuẩn cho `ml/features.py`.
   - Viết `ml/label.py` và `ml/train.py`, xuất `ml/models/meta.json`.
   - Hiệu chuẩn tầng 2: khoảng 8–10 lần tưới ngẫu nhiên mỗi Zone.
7. Thực nghiệm chéo AI và ngưỡng trên hotspot: dùng điện thoại cũ cắm sạc, nếu không có thì rút gọn còn 2 pha × 1 ngày.

## 12. Các quyết định đã chốt (25/10/2026)

| # | Quyết định |
|---|---|
| S1 | Chia giai đoạn P1/P2/P3. Mốc cứng là bắt đầu thu dữ liệu tuần 12 |
| S2 | Một `ZoneRegistry` trung tâm cùng một bộ dispatch duy nhất trong `ingest`, không dùng event bus |
| S3 | `zones` là gốc dữ liệu, ghi theo kiểu write-through. Trạng thái runtime chỉ giữ trong RAM. Tự tạo sẵn 3 Zone mặc định |
| S4 | Không có ACK sau 5 s thì `no_ack`. Gateway offline thì mọi lệnh `pending` → `no_ack` |
| S5 | Cấp `seq` bằng `$inc` trong `zones` |
| S6 | Ghép pump on/off thành event, có cờ ước lượng khi mất gói |
| S7 | Cooldown tính từ `end_ts` gần nhất, bất kể nguồn. Tưới tay = `force`, vẫn clip `dur` |
| S8 | Chưa có model thì không cho chuyển sang AI (409), không âm thầm đổi sang ngưỡng |
| S9 | Đặc trưng lúc chạy lấy từ Mongo bằng đúng `ml/features.py` dùng khi train |
| S10 | WS dùng bản tin bọc `{type, zone, ts, data}` cùng một `snapshot` khi kết nối |
| S11 | MongoDB lỗi lúc khởi động thì fail-fast; lỗi lúc chạy thì ghi log rồi bỏ bản ghi |
| S12 | Lưu `link_stats` |
| S13 | Quyết định theo mốc :00/:05, các Zone cách nhau 2 s |
| S14 | Giữ gateway, gateway nối server qua **MQTT** với broker **Mosquitto** trên laptop |
| S15 | Dùng **hotspot điện thoại**, nút dò kênh khi **gửi lỗi ở lớp MAC** (không dò theo mất beacon, xem §8.4) |
| S16 | Gateway tìm broker bằng mDNS `<laptop>.local`, dự phòng bằng IP. Cấu hình chỉnh qua console và lưu NVS |
| S17 | Broker có user/password và ACL |
| S18 | `srv/status` (LWT) + `srv/time` 60 s làm heartbeat. USB chỉ dùng cho log và console. Không đệm dữ liệu |
| S19 | Kiểm tra dữ liệu đầu vào bằng Pydantic. Bucket tự chọn. Log xoay vòng 14 ngày |
| S20 | AI: tầng 1 dùng dataset công khai rồi tinh chỉnh bằng dữ liệu thật; tầng 2 hiệu chuẩn tại chỗ; `features.py` có adapter cho từng nguồn |

## 13. Truy cập từ xa (`https://api.quochung.xyz`)

> **Thay đổi phạm vi** so với DESIGN §1.2. Trước đây "truy cập Internet" và "đăng nhập người dùng" nằm **ngoài phạm vi**. Nay dashboard được mở ra Internet để xem và điều khiển từ xa, nên bắt buộc phải có đăng nhập.

**Đường đi**: trình duyệt → Cloudflare → **Cloudflare Tunnel** `Multi-zone-smart` (service `cloudflared` trên laptop, cấu hình bằng token) → `http://127.0.0.1:8000`.

- Không cần mở cổng router hay có IP tĩnh, nên chạy được trên hotspot 4G.
- Chỉ đưa **dashboard (cổng 8000)** qua tunnel. **Mosquitto (cổng 1883) không mở ra Internet**: gateway vẫn kết nối qua hotspot.

**Cấu hình route trên Cloudflare** (Networking → Tunnels → Multi-zone-smart → Routes → `api.quochung.xyz`):
- Service đặt `HTTP` → `127.0.0.1:8000`.
- **Không dùng `localhost`**, vì trên Windows `localhost` có thể được hiểu là `::1` (IPv6), trong khi uvicorn chỉ nghe IPv4. Hệ quả là lỗi 502 Bad Gateway.

**Đăng nhập** ([server/app/auth.py](../server/app/auth.py)):
- Bật bằng `DASH_USER` / `DASH_PASS` trong `server/.env`. Nếu `DASH_PASS` trống thì không hỏi đăng nhập và log in cảnh báo; chỉ dùng cách này khi chạy trong máy.
- **Cookie phiên**: tên `irr_session`, chứa `user|hết hạn|HMAC-SHA256`, hạn 7 ngày. Cookie có cờ `HttpOnly` và `SameSite=Lax`, và có thêm `Secure` khi truy cập qua https, nhận biết bằng header `X-Forwarded-Proto` do Cloudflare gửi.
- **Khoá ký phiên** được suy ra từ `DASH_PASS`. Đổi mật khẩu là mọi phiên cũ mất hiệu lực. Khởi động lại server thì phiên vẫn còn.
- **Chưa đăng nhập** thì:
  - `/api/*` trả **401**;
  - các trang khác chuyển về `/login`;
  - WebSocket `/ws` bị đóng với mã **4401**, và dashboard tự chuyển về trang đăng nhập.
- **Đăng nhập sai** thì server chờ 1 s rồi mới trả lời, để làm chậm việc dò mật khẩu. Tham số `next` chỉ nhận đường dẫn trong site, để tránh open redirect.
- Dashboard có nút **Đăng xuất** ở góc phải, chỉ hiện khi đăng nhập đang bật.
- **Hạn chế**: chỉ có một tài khoản dùng chung và không có mã gửi qua email. Phương án mạnh hơn nhưng có thể phải khai báo thẻ là Cloudflare Access (gói Zero Trust Free, ≤ 50 người dùng). Server không cần sửa gì nếu sau này bật thêm Access.
