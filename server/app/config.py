"""Cấu hình đọc từ server/.env (docs/SERVER.md §Cấu hình)."""
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict

SERVER_DIR = Path(__file__).resolve().parent.parent
REPO_DIR = SERVER_DIR.parent

ZONES = (1, 2, 3)

DECISION_PERIOD_S = 300      # quyết định mỗi 5 phút, đúng mốc :00/:05 (DESIGN §7.3)
DECISION_STAGGER_S = 2       # giãn cách giữa các Zone để hàng đợi lệnh gateway không dồn
STALE_S = 120                # dữ liệu cũ hơn 2 phút thì không quyết định
ACK_TIMEOUT_S = 5            # không có ACK sau 5 s -> no_ack
TIME_PERIOD_S = 60           # irr/srv/time: đồng bộ giờ + heartbeat
MQTT_RETRY_S = 5
AFTER_JOB_PERIOD_S = 60      # job soil_after_10m
AFTER_WINDOW_MIN = (8, 12)   # soil_after_10m = trung bình độ ẩm trong [+8, +12] phút sau khi bơm tắt
COLLECT_DUR_RANGE = (5, 25)  # collect_mode: thời lượng ngẫu nhiên để có dữ liệu cho tầng 2
AI_DUR_MIN_S = 3
AI_P_THRESHOLD = 0.5
EVENT_CLOSE_GRACE_S = 5      # tel pump=0 sau khi bơm bật ít nhất 5 s mới coi là mất gói pump off


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=SERVER_DIR / ".env", extra="ignore")

    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_user: str = "server"
    mqtt_pass: str = ""
    mongo_uri: str = "mongodb://localhost:27017"  # "memory://" = lưu trong RAM, chỉ để chạy thử
    db_name: str = "irrigation"
    models_dir: Path = REPO_DIR / "ml" / "models"
    log_dir: Path = SERVER_DIR / "logs"
    http_host: str = "0.0.0.0"
    http_port: int = 8000
    dash_user: str = "admin"
    dash_pass: str = ""  # rỗng = không hỏi đăng nhập; BẮT BUỘC đặt khi mở dashboard ra Internet
