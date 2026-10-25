"""Dựng `irrigation_events` từ bản tin pump on/off và điền soil_after_10m (DESIGN §8.2)."""
from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timedelta, timezone
from typing import TYPE_CHECKING

from .config import AFTER_JOB_PERIOD_S, AFTER_WINDOW_MIN, EVENT_CLOSE_GRACE_S

if TYPE_CHECKING:
    from .db import Store
    from .registry import ZoneRegistry
    from .ws import WsHub

log = logging.getLogger(__name__)

SOURCE = {"cmd": "cmd", "force": "manual", "fallback": "fallback"}


class EventTracker:
    def __init__(self, store: "Store", registry: "ZoneRegistry", ws: "WsHub") -> None:
        self.store = store
        self.registry = registry
        self.ws = ws

    async def on_pump(self, zone_id: int, msg: dict, now: datetime) -> None:
        zone = self.registry.zones[zone_id]
        src = msg.get("src", "cmd")
        cmd_seq = msg.get("cmd_seq") or None
        open_ev = await self.store.open_event(zone_id)
        if msg.get("on"):
            if open_ev:  # mất gói off của lần trước
                await self._close(open_ev, now, end_estimated=True)
            decided_by = await self._decided_by(zone_id, src, cmd_seq)
            tel = zone.state.tel or {}
            doc = {"zone_id": zone_id, "start_ts": now, "end_ts": None, "duration_s": None,
                   "source": SOURCE.get(src, src), "decided_by": decided_by, "cmd_seq": cmd_seq,
                   "soil_before": tel.get("soil")}
            doc["_id"] = await self.store.insert_event(doc)
            zone.state.pump = True
            await self.ws.broadcast("pump", zone_id, {"on": 1, "event": doc})
            return
        dur = msg.get("dur")
        if open_ev is None:  # mất gói on: dựng lại từ dur
            decided_by = await self._decided_by(zone_id, src, cmd_seq)
            start = now - timedelta(seconds=dur or 0)
            open_ev = {"zone_id": zone_id, "start_ts": start, "end_ts": None, "duration_s": None,
                       "source": SOURCE.get(src, src), "decided_by": decided_by, "cmd_seq": cmd_seq,
                       "soil_before": None, "start_estimated": True}
            open_ev["_id"] = await self.store.insert_event(open_ev)
        await self._close(open_ev, now, duration_s=dur)

    async def on_tel(self, zone_id: int, pump: int, now: datetime) -> None:
        """tel báo bơm tắt nhưng event còn mở: gói pump off đã mất."""
        if pump:
            return
        open_ev = await self.store.open_event(zone_id)
        if open_ev and (now - open_ev["start_ts"]).total_seconds() > EVENT_CLOSE_GRACE_S:
            pump_max = self.registry.zones[zone_id].config.pump_max_s
            end = min(now, open_ev["start_ts"] + timedelta(seconds=pump_max))
            await self._close(open_ev, end, end_estimated=True)

    async def _decided_by(self, zone_id: int, src: str, cmd_seq: int | None) -> str:
        if src == "fallback":
            return "node"
        if src == "force":
            return "manual"
        cmd = await self.store.find_command(zone_id, cmd_seq) if cmd_seq else None
        return cmd["decided_by"] if cmd else "unknown"

    async def _close(self, ev: dict, end: datetime, duration_s: float | None = None,
                     end_estimated: bool = False) -> None:
        if duration_s is None:
            duration_s = round((end - ev["start_ts"]).total_seconds())
        fields = {"end_ts": end, "duration_s": duration_s}
        if end_estimated:
            fields["end_estimated"] = True
        await self.store.update_event(ev["_id"], fields)
        zone = self.registry.zones[ev["zone_id"]]
        zone.state.pump = False
        if zone.state.last_water_end is None or end > zone.state.last_water_end:
            zone.state.last_water_end = end
        await self.ws.broadcast("pump", ev["zone_id"], {"on": 0, "event": {**ev, **fields}})

    async def fill_soil_after(self, now: datetime | None = None) -> int:
        now = now or datetime.now(timezone.utc)
        lo, hi = AFTER_WINDOW_MIN
        n = 0
        for ev in await self.store.events_pending_after(now - timedelta(minutes=hi)):
            avg = await self.store.avg_soil(ev["zone_id"], ev["end_ts"] + timedelta(minutes=lo),
                                            ev["end_ts"] + timedelta(minutes=hi))
            fields = {"soil_after_10m": round(avg, 2) if avg is not None else None}
            if avg is None:
                fields["after_missing"] = True
            await self.store.update_event(ev["_id"], fields)
            n += 1
        return n

    async def run_after_job(self) -> None:
        while True:
            try:
                await self.fill_soil_after()
            except Exception:
                log.exception("Job soil_after_10m lỗi")
            await asyncio.sleep(AFTER_JOB_PERIOD_S)
