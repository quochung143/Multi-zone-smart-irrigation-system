"""FastAPI app: nối các module, chạy task MQTT / decision / soil_after_10m trong lifespan (docs/SERVER.md)."""
from __future__ import annotations

import asyncio
import logging
import sys
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from . import api, auth, ws
from .commands import CommandManager
from .config import Settings
from .db import Store, make_store
from .decision import DecisionEngine
from .events import EventTracker
from .ingest import Ingest
from .ml_runtime import ModelRuntime
from .mqtt_link import MqttLink
from .registry import ZoneRegistry
from .ws import WsHub

log = logging.getLogger("app")


def setup_logging(log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    file = TimedRotatingFileHandler(log_dir / "server.log", when="midnight", backupCount=14, encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):  # console Windows mặc định cp1252, log có tiếng Việt
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    console = logging.StreamHandler()
    root = logging.getLogger()
    for h in (file, console):
        h.setFormatter(fmt)
        root.addHandler(h)
    root.setLevel(logging.INFO)


class Services:
    def __init__(self, settings: Settings, store: Store) -> None:
        self.settings = settings
        self.store = store
        self.ws = WsHub()
        self.registry = ZoneRegistry(store)
        self.ml = ModelRuntime(settings.models_dir)
        self.link = MqttLink(settings, on_message=self._on_message, on_disconnect=self._on_link_down)
        self.commands = CommandManager(store, self.registry, self.link, self.ws)
        self.events = EventTracker(store, self.registry, self.ws)
        self.ingest = Ingest(store, self.registry, self.commands, self.events, self.link, self.ws)
        self.decision = DecisionEngine(store, self.registry, self.commands, self.ml, self.ws)
        self.srv_session = None

    async def _on_message(self, topic: str, payload: bytes, retain: bool) -> None:
        await self.ingest.handle(topic, payload, retain=retain)

    async def _on_link_down(self) -> None:
        await self.ingest.on_link_down()

    def snapshot(self) -> dict:
        return {"zones": self.registry.snapshot(), "gateway": {"online": self.registry.gw_online},
                "mqtt": self.link.connected, "model": self.ml.version}

    async def start(self) -> list[asyncio.Task]:
        await self.store.init()
        await self.registry.load()
        now = datetime.now(timezone.utc)
        # Phiên còn mở từ lần chạy trước (server crash): đóng tại thời điểm có dữ liệu cuối
        last = await self.store.last_reading_ts() or now
        for kind in ("gw_sessions", "srv_sessions"):
            await self.store.close_dangling_sessions(kind, last)
        self.srv_session = await self.store.open_session("srv_sessions", now)
        return [asyncio.create_task(coro) for coro in
                (self.link.run(), self.decision.run(), self.events.run_after_job())]

    async def stop(self, tasks: list[asyncio.Task]) -> None:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        now = datetime.now(timezone.utc)
        if self.ingest.gw_session is not None:
            await self.store.close_session("gw_sessions", self.ingest.gw_session, now)
        await self.store.close_session("srv_sessions", self.srv_session, now)
        await self.store.close()


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        setup_logging(settings.log_dir)
        svc = Services(settings, make_store(settings.mongo_uri, settings.db_name))
        tasks = await svc.start()
        app.state.svc = svc
        log.info("Server sẵn sàng: http://localhost:%d", settings.http_port)
        if not settings.dash_pass:
            log.warning("DASH_PASS trống: dashboard KHÔNG hỏi đăng nhập, đừng mở ra Internet")
        try:
            yield
        finally:
            await svc.stop(tasks)

    app = FastAPI(title="Multi-zone smart irrigation", lifespan=lifespan)
    app.state.settings = settings
    app.middleware("http")(auth.middleware)
    app.include_router(auth.router)
    app.include_router(api.router)
    app.include_router(ws.router)
    app.mount("/", StaticFiles(directory=Path(__file__).parent.parent / "static", html=True), name="static")
    return app


app = create_app()
