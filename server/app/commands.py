"""Lệnh xuống nút: cấp seq, lưu `commands` (pending), cập nhật khi có ACK; không có ACK sau 5 s -> no_ack."""
from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timezone
from typing import TYPE_CHECKING

from .config import ACK_TIMEOUT_S
from .mqtt_link import LinkDown, cmd_topic

if TYPE_CHECKING:
    from .db import Store
    from .mqtt_link import MqttLink
    from .registry import ZoneRegistry
    from .ws import WsHub

log = logging.getLogger(__name__)


class GatewayOffline(RuntimeError):
    pass


class CommandManager:
    def __init__(self, store: "Store", registry: "ZoneRegistry", link: "MqttLink", ws: "WsHub") -> None:
        self.store = store
        self.registry = registry
        self.link = link
        self.ws = ws
        self.pending: dict[tuple[int, int], asyncio.Task] = {}

    async def water(self, zone_id: int, dur: int, decided_by: str, force: bool = False) -> dict:
        payload = {"t": "cmd", "dur": dur}
        if force:
            payload["force"] = 1
        return await self._send(zone_id, "water", payload, decided_by, dur=dur)

    async def stop(self, zone_id: int, decided_by: str = "manual") -> dict:
        return await self._send(zone_id, "stop", {"t": "stop"}, decided_by)

    async def cfg(self, zone_id: int) -> dict:
        c = self.registry.zones[zone_id].config
        payload = {"t": "cfg", "low": c.soil_low, "max": c.pump_max_s, "fb": c.fallback_s}
        return await self._send(zone_id, "cfg", payload, "server")

    async def _send(self, zone_id: int, type_: str, payload: dict, decided_by: str, dur: int | None = None) -> dict:
        if not self.registry.gw_online:
            raise GatewayOffline("gateway offline")
        seq = await self.store.next_seq(zone_id)
        doc = {"zone_id": zone_id, "seq": seq, "type": type_, "dur": dur, "force": bool(payload.get("force")),
               "created_ts": datetime.now(timezone.utc), "status": "pending", "ack_ts": None,
               "decided_by": decided_by}
        await self.store.insert_command(doc)
        try:
            await self.link.publish(cmd_topic(zone_id), {**payload, "zone": zone_id, "seq": seq})
        except LinkDown:
            await self._finish(zone_id, seq, "no_ack")
            raise GatewayOffline("mất kết nối broker") from None
        self.pending[(zone_id, seq)] = asyncio.create_task(self._expire(zone_id, seq))
        await self.ws.broadcast("command", zone_id, doc)
        return doc

    async def _expire(self, zone_id: int, seq: int) -> None:
        await asyncio.sleep(ACK_TIMEOUT_S)
        self.pending.pop((zone_id, seq), None)
        log.warning("Zone %d lệnh seq %d: không có ACK sau %d s", zone_id, seq, ACK_TIMEOUT_S)
        await self._finish(zone_id, seq, "no_ack")

    async def on_ack(self, zone_id: int, seq: int, status: str) -> None:
        task = self.pending.pop((zone_id, seq), None)
        if task is None:
            log.info("Zone %d ACK seq %d (%s) không khớp lệnh đang chờ", zone_id, seq, status)
        else:
            task.cancel()
        await self._finish(zone_id, seq, status)

    async def on_gw_offline(self) -> None:
        for (zone_id, seq), task in list(self.pending.items()):
            task.cancel()
            await self._finish(zone_id, seq, "no_ack")
        self.pending.clear()

    async def _finish(self, zone_id: int, seq: int, status: str) -> None:
        fields = {"status": status, "ack_ts": datetime.now(timezone.utc)}
        await self.store.update_command(zone_id, seq, fields)
        await self.ws.broadcast("ack", zone_id, {"seq": seq, **fields})
