# Multi-zone smart irrigation system

Hệ thống tưới cây thông minh đa vùng: 3 nút ESP32 (độ ẩm đất, SHT30, bơm) giao tiếp ESP-NOW với 1 ESP32 gateway; gateway nối USB-Serial tới server cục bộ (FastAPI + MongoDB + AI Random Forest/XGBoost + web dashboard).

Thiết kế chi tiết: [docs/DESIGN.md](docs/DESIGN.md)

## Cấu trúc

| Thư mục | Nội dung |
|---|---|
| `firmware/` | PlatformIO: env `node_z1..3`, `gateway`; `lib/protocol` dùng chung |
| `server/` | FastAPI backend, serial bridge, decision engine, dashboard (`static/`) |
| `ml/` | Gán nhãn, đặc trưng, huấn luyện và đánh giá mô hình |
| `docs/` | Tài liệu thiết kế, kế hoạch |

## Chạy nhanh

```bash
# Firmware
cd firmware
pio run -e node_z1 -t upload
pio run -e gateway -t upload

# Server
cd server
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
copy .env.example .env
uvicorn app.main:app --reload
```
