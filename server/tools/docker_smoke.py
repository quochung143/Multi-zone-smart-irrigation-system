"""Kiểm tra nhanh stack Docker: chạy TRONG container server (đã có MongoDB thật + gateway gửi dữ liệu).

    docker compose exec server python tools/docker_smoke.py

Tạo phiên đăng nhập từ DASH_PASS trong môi trường container (không cần gõ mật khẩu), tưới tay Zone 1
trong 3 s rồi đọc lại readings (thô + gom 5 phút), events, commands, stats qua REST.
"""
import json
import time
import urllib.request
from types import SimpleNamespace

from app import auth
from app.config import Settings

BASE = "http://127.0.0.1:8000"
s = Settings()
cookie = ""
if s.dash_pass:
    fake_app = SimpleNamespace(state=SimpleNamespace(settings=s))
    cookie = f"{auth.COOKIE}={auth.make_token(fake_app, s.dash_user)}"


def call(method, path, body=None):
    req = urllib.request.Request(BASE + path, method=method, data=json.dumps(body).encode() if body else None,
                                 headers={"Cookie": cookie, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


print("health:", call("GET", "/api/health"))
cmd = call("POST", "/api/zones/1/water", {"dur": 3})
print("water:", cmd["seq"], cmd["status"])
time.sleep(6)
raw = call("GET", "/api/readings?zone=1")
print("readings tho:", len(raw["rows"]), "bucket", raw["bucket_s"])
day = call("GET", "/api/readings?from=2026-01-01T00:00:00Z&to=2026-12-31T00:00:00Z&bucket=300")
print("readings gom 5 phut:", len(day["rows"]), day["rows"][:1])
print("events:", [(e["zone_id"], e["decided_by"], e["duration_s"]) for e in call("GET", "/api/events")])
print("commands:", [(c["zone_id"], c["type"], c["status"]) for c in call("GET", "/api/commands?limit=10")])
print("stats z1:", call("GET", "/api/stats?zone=1"))
