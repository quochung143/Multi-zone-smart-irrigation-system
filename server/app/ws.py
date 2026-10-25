"""WebSocket /ws: đẩy sự kiện cho dashboard dưới dạng {type, zone, ts, data} (docs/SERVER.md §WebSocket)."""
from __future__ import annotations

import json
import logging
from datetime import datetime, timezone

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from . import auth

log = logging.getLogger(__name__)
router = APIRouter()


def to_json(obj) -> str:
    def default(o):
        if isinstance(o, datetime):
            return o.isoformat()
        return str(o)
    return json.dumps(obj, default=default, ensure_ascii=False)


class WsHub:
    def __init__(self) -> None:
        self.clients: set[WebSocket] = set()

    async def broadcast(self, type_: str, zone: int | None, data) -> None:
        if not self.clients:
            return
        text = to_json({"type": type_, "zone": zone, "ts": datetime.now(timezone.utc), "data": data})
        for ws in list(self.clients):
            try:
                await ws.send_text(text)
            except Exception:  # client đã đóng: bỏ khỏi danh sách
                self.clients.discard(ws)


@router.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    if not auth.allowed(ws.app, ws.cookies):
        # Phải accept rồi mới đóng: đóng trước handshake thì trình duyệt chỉ thấy mã 1006
        await ws.close(code=4401)  # dashboard thấy mã này thì chuyển về /login
        return
    svc = ws.app.state.svc
    await ws.send_text(to_json({"type": "snapshot", "zone": None, "ts": datetime.now(timezone.utc),
                                "data": svc.snapshot()}))
    svc.ws.clients.add(ws)
    try:
        while True:
            await ws.receive_text()  # dashboard không gửi gì; chỉ để phát hiện ngắt kết nối
    except WebSocketDisconnect:
        pass
    finally:
        svc.ws.clients.discard(ws)
