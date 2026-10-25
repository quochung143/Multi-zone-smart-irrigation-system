"""Dispatch bản tin MQTT theo topic -> registry / commands / events / db / ws. Giờ server là chuẩn (ts = now)."""
from __future__ import annotations

import json
import logging
from datetime import datetime, timezone
from typing import TYPE_CHECKING

from .commands import GatewayOffline
from .mqtt_link import GW_STATUS, UP_KINDS

if TYPE_CHECKING:
    from .commands import CommandManager
    from .db import Store
    from .events import EventTracker
    from .mqtt_link import MqttLink
    from .registry import ZoneRegistry
    from .ws import WsHub

log = logging.getLogger(__name__)

READING_FIELDS = ("seq", "soil", "soil_raw", "temp", "hum", "pump", "flags", "rssi", "up")


class Ingest:
    def __init__(self, store: "Store", registry: "ZoneRegistry", commands: "CommandManager",
                 events: "EventTracker", link: "MqttLink", ws: "WsHub") -> None:
        self.store = store
        self.registry = registry
        self.commands = commands
        self.events = events
        self.link = link
        self.ws = ws
        self.gw_session = None

    async def handle(self, topic: str, payload: bytes, now: datetime | None = None, retain: bool = False) -> None:
        now = now or datetime.now(timezone.utc)
        if topic == GW_STATUS:
            return await self.on_gw_status(payload.decode(errors="replace").strip() == "online", now)
        parts = topic.split("/")
        if len(parts) != 4 or parts[0] != "irr" or parts[1] != "z" or not parts[2].isdigit():
            return log.warning("Bỏ topic lạ %s", topic)
        zone_id, kind = int(parts[2]), parts[3]
        if self.registry.get(zone_id) is None:
            return log.warning("Bỏ bản tin của zone không tồn tại: %s", topic)
        try:
            msg = json.loads(payload)
        except json.JSONDecodeError:
            return log.warning("JSON lỗi trên %s: %r", topic, payload[:100])
        if kind not in UP_KINDS or not isinstance(msg, dict):
            return log.warning("Bỏ bản tin %s: %r", topic, payload[:100])
        if retain and kind == "node" and msg.get("online"):
            return  # node=online còn retain từ phiên trước có thể đã cũ: đợi gói thật từ nút
        await getattr(self, f"on_{kind}")(zone_id, msg, now)

    # ---- gateway ----
    async def on_gw_status(self, online: bool, now: datetime) -> None:
        if online == self.registry.gw_online:
            return
        self.registry.gw_online = online
        log.info("Gateway %s", "online" if online else "offline")
        if online:
            self.gw_session = await self.store.open_session("gw_sessions", now)
            await self.resync()
        else:
            if self.gw_session is not None:
                await self.store.close_session("gw_sessions", self.gw_session, now)
                self.gw_session = None
            await self.commands.on_gw_offline()
            for zone_id, zone in self.registry.zones.items():  # mất gateway thì không còn biết nút sống hay không
                if zone.state.online:
                    await self.on_node(zone_id, {"online": False}, now)
        await self.ws.broadcast("gateway", None, {"online": online})

    async def on_link_down(self) -> None:
        """Mất broker: không còn biết gì về gateway, coi như offline."""
        await self.on_gw_status(False, datetime.now(timezone.utc))

    async def resync(self) -> None:
        """Gateway vừa online: gửi time rồi cfg cho cả 3 Zone."""
        try:
            await self.link.publish_time()
            for zone_id in self.registry.zones:
                await self.commands.cfg(zone_id)
        except GatewayOffline as e:
            log.warning("Resync dở dang: %s", e)

    # ---- nút ----
    async def _seen(self, zone_id: int, now: datetime) -> None:
        st = self.registry.zones[zone_id].state
        st.last_seen = now
        if not st.online:  # có gói từ nút nghĩa là nút online, dù chưa nhận bản tin node
            await self.on_node(zone_id, {"online": True}, now)

    async def on_tel(self, zone_id: int, msg: dict, now: datetime) -> None:
        doc = {"ts": now, "zone_id": zone_id, "node_mode": msg.get("mode"),
               **{k: msg.get(k) for k in READING_FIELDS}}
        await self.store.insert_reading(doc)
        await self._seen(zone_id, now)
        st = self.registry.zones[zone_id].state
        st.tel = {**doc, "mode": msg.get("mode")}
        await self.events.on_tel(zone_id, msg.get("pump", 0), now)
        await self.ws.broadcast("tel", zone_id, st.tel)

    async def on_pump(self, zone_id: int, msg: dict, now: datetime) -> None:
        await self._seen(zone_id, now)
        await self.events.on_pump(zone_id, msg, now)

    async def on_ack(self, zone_id: int, msg: dict, now: datetime) -> None:
        if isinstance(msg.get("seq"), int):
            await self.commands.on_ack(zone_id, msg["seq"], str(msg.get("status")))

    async def on_node(self, zone_id: int, msg: dict, now: datetime) -> None:
        online = bool(msg.get("online"))
        st = self.registry.zones[zone_id].state
        if online == st.online:
            return
        st.online = online
        await self.store.insert_node_status({"ts": now, "zone_id": zone_id, "online": online})
        await self.ws.broadcast("node", zone_id, {"online": online, "last_seen": st.last_seen})
        log.info("Zone %d %s", zone_id, "online" if online else "offline")
        if online and self.registry.gw_online:
            try:
                await self.commands.cfg(zone_id)
            except GatewayOffline as e:
                log.warning("Không gửi được cfg Zone %d: %s", zone_id, e)

    async def on_stat(self, zone_id: int, msg: dict, now: datetime) -> None:
        doc = {"ts": now, "zone_id": zone_id, **{k: msg.get(k) for k in ("rx", "lost", "dup", "bad")}}
        await self.store.insert_link_stat(doc)
        self.registry.zones[zone_id].state.link_stat = doc
        await self.ws.broadcast("stat", zone_id, doc)
