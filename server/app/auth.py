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
# Ảnh nền + favicon cần tải được ngay ở trang đăng nhập
PUBLIC_PATHS = {"/login", "/logout", "/login-bg.webp", "/favicon.svg", "/favicon.ico", "/apple-touch-icon.png"}


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
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="icon" href="/favicon.ico" sizes="any">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<meta name="theme-color" content="#0a4225">
<style>
  :root {{ --deep:#0a4225; --forest:#145a32; --emerald:#1e8449; --lime:#a3e635; --muted:#5b6b62; --line:#e2e8f0;
          --bad:#be123c; --bad-soft:#fff1f2; color-scheme:light; }}
  body {{ margin:0; min-height:100vh; display:grid; grid-template-columns:minmax(0, 1fr) minmax(420px, 40%);
         color:#143221; background:#efe5c8; font:14px/1.5 Inter, system-ui, "Segoe UI", Roboto, sans-serif; }}
  /* ?v=: đổi khi thay ảnh để Cloudflare không trả ảnh cũ */
  .art {{ background:#c9b37a url("/login-bg.webp?v=2") center 65% / cover no-repeat; }}
  .side {{ display:grid; place-items:center; padding:24px 16px;
          background:radial-gradient(circle at 80% 15%, rgb(163 230 53 / .14) 0, transparent 45%),
                     linear-gradient(160deg, #f3ebd2, #e8dcb6); }}
  @media (max-width: 860px) {{
    body {{ display:block; background:#c9b37a url("/login-bg.webp?v=2") center 85% / cover no-repeat fixed; }}
    .art {{ display:none; }}
    .side {{ min-height:100vh; box-sizing:border-box; background:rgb(60 50 20 / .12); }}
  }}
  form {{ box-sizing:border-box; width:100%; max-width:380px; background:#fff; border:1px solid var(--line); border-radius:0; overflow:hidden;
         display:grid; gap:14px; padding:0 28px 28px;
         box-shadow:0 20px 35px -5px rgb(10 66 37 / .12), 0 8px 20px -2px rgb(10 66 37 / .05); }}
  .strip {{ height:6px; margin:0 -28px 10px; background:linear-gradient(90deg, var(--deep), var(--lime), #ea580c); }}
  .brand {{ display:flex; align-items:center; gap:12px; margin-bottom:6px; }}
  .logo {{ width:44px; height:44px; padding:11px; border-radius:0; flex:none; box-sizing:border-box;
          background:linear-gradient(135deg, var(--deep), var(--emerald)); fill:none; stroke:var(--lime);
          stroke-width:2; stroke-linecap:round; stroke-linejoin:round; box-shadow:0 6px 14px -4px rgb(10 66 37 / .35); }}
  h1 {{ font-size:18px; font-weight:800; margin:0; color:var(--deep); letter-spacing:-0.015em; }}
  p {{ margin:0; color:var(--muted); font-size:12px; }}
  label {{ display:grid; gap:6px; font-size:12px; font-weight:700; color:#4b5563; }}
  input {{ font:inherit; color:var(--deep); background:#f9fafb; border:1px solid var(--line); border-radius:0;
          padding:0 12px; min-height:44px; transition:border-color .2s, box-shadow .2s; }}
  input:focus {{ outline:none; border-color:var(--emerald); background:#fff; box-shadow:0 0 0 3px rgb(30 132 73 / .12); }}
  :focus-visible {{ outline:2px solid var(--emerald); outline-offset:2px; }}
  button {{ font:inherit; font-weight:700; background:var(--deep); color:#fff; border:0; border-radius:0;
           min-height:44px; cursor:pointer; margin-top:6px; transition:background-color .2s; }}
  button:hover {{ background:var(--forest); }}
  .err {{ color:var(--bad); background:var(--bad-soft); border:1px solid #fecdd3; padding:8px 12px; border-radius:0; font-weight:600; }}
</style>
</head>
<body>
<div class="art" role="img" aria-label="Tranh vẽ vân gỗ với muông thú và cây cỏ"></div>
<main class="side">
<form method="post" action="/login">
  <div class="strip" aria-hidden="true"></div>
  <div class="brand">
    <svg class="logo" viewBox="0 0 24 24" aria-hidden="true"><path d="M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10Z"/><path d="M2 21c0-3 1.85-5.36 5.08-6C9.5 14.52 12 13 13 12"/></svg>
    <div><h1>Smart Irrigation</h1><p>Hệ thống tưới nước thông minh đa vùng</p></div>
  </div>
  {error}
  <input type="hidden" name="next" value="{next}">
  <label>Tài khoản<input name="user" autocomplete="username" required value="{user}"></label>
  <label>Mật khẩu<input name="password" type="password" autocomplete="current-password" required autofocus></label>
  <button>Đăng nhập</button>
</form>
</main>
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
