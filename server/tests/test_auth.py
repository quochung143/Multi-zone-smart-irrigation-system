import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app import auth
from app.config import Settings
from app.main import create_app


def client(env, password: str) -> TestClient:
    app = create_app(Settings(dash_user="admin", dash_pass=password))
    app.state.svc = env  # không chạy lifespan (MQTT/Mongo); dùng dịch vụ giả của conftest
    return TestClient(app, follow_redirects=False)


def login(c: TestClient, password="bimat", nxt="/"):
    return c.post("/login", data={"user": "admin", "password": password, "next": nxt})


def test_disabled_without_password(env):
    c = client(env, "")
    assert c.get("/api/zones").status_code == 200
    assert c.get("/api/health").json()["auth"] is False


def test_requires_login(env):
    c = client(env, "bimat")
    assert c.get("/api/zones").status_code == 401
    r = c.get("/")
    assert r.status_code == 303 and r.headers["location"] == "/login?next=/"
    assert c.get("/login").status_code == 200
    with pytest.raises(WebSocketDisconnect) as e:
        with c.websocket_connect("/ws") as sock:
            sock.receive_json()
    assert e.value.code == 4401


def test_login_flow(env, monkeypatch):
    monkeypatch.setattr(auth, "FAIL_DELAY_S", 0)
    c = client(env, "bimat")
    assert login(c, "sai").status_code == 401
    r = login(c, nxt="//evil.example")
    assert r.status_code == 303 and r.headers["location"] == "/"
    assert "httponly" in r.headers["set-cookie"].lower()
    assert c.get("/api/zones").status_code == 200
    assert c.get("/api/health").json()["auth"] is True
    with c.websocket_connect("/ws") as sock:
        assert sock.receive_json()["type"] == "snapshot"
    c.post("/logout")
    assert c.get("/api/zones").status_code == 401


def test_secure_cookie_behind_tunnel(env):
    c = client(env, "bimat")
    r = c.post("/login", data={"user": "admin", "password": "bimat"}, headers={"x-forwarded-proto": "https"})
    assert "secure" in r.headers["set-cookie"].lower()


def test_token_tamper_expiry_and_password_change(env):
    c = client(env, "bimat")
    app = c.app
    tok = auth.make_token(app, "admin", now=1000)
    assert auth.valid_token(app, tok, now=1001)
    assert not auth.valid_token(app, tok, now=1000 + auth.SESSION_S + 1)
    assert not auth.valid_token(app, tok.replace("admin", "khach"), now=1001)
    app.state.settings = Settings(dash_user="admin", dash_pass="moi")
    assert not auth.valid_token(app, tok, now=1001)
