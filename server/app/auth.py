"""Đăng nhập dashboard khi mở ra Internet (docs/SERVER.md §13).

- Bật khi DASH_PASS khác rỗng; rỗng thì không hỏi mật khẩu (chạy thử trong máy).
- Phiên = cookie "irr_session" = user|hết hạn|HMAC-SHA256. Khoá ký suy ra từ DASH_PASS:
  đổi mật khẩu là mọi phiên cũ mất hiệu lực, khởi động lại server thì phiên vẫn còn.
- Chưa đăng nhập: /api/* trả 401, trang khác chuyển về /login, WebSocket /ws bị đóng mã 4401.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import html
import time
from urllib.parse import quote

from fastapi import APIRouter, Form, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

router = APIRouter()

COOKIE = "irr_session"
SESSION_S = 7 * 24 * 3600
FAIL_DELAY_S = 1.0  # làm chậm dò mật khẩu
PUBLIC_PATHS = {"/login", "/logout"}


def _settings(app):
    return getattr(app.state, "settings", None)


def enabled(app) -> bool:
    s = _settings(app)
    return bool(s and s.dash_pass)


def _key(app) -> bytes:
    return hashlib.sha256(b"irr-session|" + _settings(app).dash_pass.encode()).digest()


def make_token(app, user: str, now: float | None = None) -> str:
    exp = int((now or time.time()) + SESSION_S)
    body = f"{user}|{exp}"
    sig = hmac.new(_key(app), body.encode(), hashlib.sha256).hexdigest()
    return f"{body}|{sig}"


def valid_token(app, token: str | None, now: float | None = None) -> bool:
    if not token or token.count("|") != 2:
        return False
    user, exp, sig = token.split("|")
    good = hmac.new(_key(app), f"{user}|{exp}".encode(), hashlib.sha256).hexdigest()
    return (hmac.compare_digest(sig, good) and exp.isdigit() and int(exp) > (now or time.time())
            and user == _settings(app).dash_user)


def allowed(app, cookies) -> bool:
    return not enabled(app) or valid_token(app, cookies.get(COOKIE))


async def middleware(request: Request, call_next):
    if allowed(request.app, request.cookies) or request.url.path in PUBLIC_PATHS:
        return await call_next(request)
    if request.url.path.startswith("/api/"):
        return JSONResponse({"detail": "chưa đăng nhập"}, status_code=401)
    return RedirectResponse(f"/login?next={quote(request.url.path)}", status_code=303)


def _safe_next(nxt: str | None) -> str:
    # Chỉ cho chuyển về đường dẫn trong site, tránh open redirect
    return nxt if nxt and nxt.startswith("/") and not nxt.startswith("//") else "/"


LOGIN_PAGE = """<!doctype html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Đăng nhập · Tưới cây đa vùng</title>
<style>
  :root {{ --bg:#f4f6f3; --card:#fff; --text:#1d2420; --muted:#64706a; --line:#dde3df; --accent:#1f7a4d; --bad:#c2410c; }}
  @media (prefers-color-scheme: dark) {{
    :root {{ --bg:#121614; --card:#1b211e; --text:#e6ece8; --muted:#94a19a; --line:#2c3530; --accent:#34a46b; --bad:#f07a45; }}
  }}
  body {{ margin:0; min-height:100vh; display:grid; place-items:center; background:var(--bg); color:var(--text);
         font:14px/1.45 system-ui, "Segoe UI", Roboto, sans-serif; padding:16px; box-sizing:border-box; }}
  form {{ width:100%; max-width:320px; background:var(--card); border:1px solid var(--line); border-radius:10px;
         padding:20px; display:grid; gap:12px; }}
  h1 {{ font-size:17px; margin:0; }}
  p {{ margin:0; color:var(--muted); }}
  label {{ display:grid; gap:4px; font-size:13px; color:var(--muted); }}
  input {{ font:inherit; color:var(--text); background:var(--bg); border:1px solid var(--line); border-radius:6px;
          padding:8px; }}
  button {{ font:inherit; background:var(--accent); color:#fff; border:0; border-radius:6px; padding:9px; cursor:pointer; }}
  .err {{ color:var(--bad); }}
</style>
</head>
<body>
<form method="post" action="/login">
  <h1>Tưới cây thông minh đa vùng</h1>
  <p>Đăng nhập để xem và điều khiển.</p>
  {error}
  <input type="hidden" name="next" value="{next}">
  <label>Tài khoản<input name="user" autocomplete="username" required value="{user}"></label>
  <label>Mật khẩu<input name="password" type="password" autocomplete="current-password" required autofocus></label>
  <button>Đăng nhập</button>
</form>
</body>
</html>"""


def _page(nxt: str, user: str = "", error: str = "", status: int = 200) -> HTMLResponse:
    err = f'<p class="err">{html.escape(error)}</p>' if error else ""
    return HTMLResponse(LOGIN_PAGE.format(error=err, next=html.escape(_safe_next(nxt)), user=html.escape(user)),
                        status_code=status)


@router.get("/login")
async def login_page(request: Request, next: str = "/"):
    if allowed(request.app, request.cookies):
        return RedirectResponse(_safe_next(next), status_code=303)
    return _page(next)


@router.post("/login")
async def login(request: Request, user: str = Form(""), password: str = Form(""), next: str = Form("/")):
    if not enabled(request.app):
        return RedirectResponse(_safe_next(next), status_code=303)
    s = _settings(request.app)
    ok = hmac.compare_digest(user.encode(), s.dash_user.encode()) & hmac.compare_digest(
        password.encode(), s.dash_pass.encode())
    if not ok:
        await asyncio.sleep(FAIL_DELAY_S)
        return _page(next, user, "Sai tài khoản hoặc mật khẩu", status=401)
    resp = RedirectResponse(_safe_next(next), status_code=303)
    # Qua Cloudflare Tunnel thì trình duyệt dùng https dù server chỉ nhận http
    secure = request.headers.get("x-forwarded-proto", request.url.scheme) == "https"
    resp.set_cookie(COOKIE, make_token(request.app, s.dash_user), max_age=SESSION_S, httponly=True,
                    secure=secure, samesite="lax")
    return resp


@router.post("/logout")
async def logout():
    resp: Response = RedirectResponse("/login", status_code=303)
    resp.delete_cookie(COOKIE)
    return resp
