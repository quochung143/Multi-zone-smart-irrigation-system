"""Decision engine: mỗi 5 phút (mốc :00/:05), mỗi Zone theo mode của nó (DESIGN §7.3).

Mọi quyết định, kể cả không tưới, đều ghi vào `predictions` với action_taken:
water | none | skip_manual | skip_offline | skip_stale | skip_sensor | skip_no_model |
skip_pump_on | skip_cooldown | skip_gw_offline
"""
from __future__ import annotations

import asyncio
import logging
import random
import time
from datetime import datetime, timezone
from typing import TYPE_CHECKING

from .commands import GatewayOffline
from .config import (AI_DUR_MIN_S, AI_P_THRESHOLD, COLLECT_DUR_RANGE, DECISION_PERIOD_S, DECISION_STAGGER_S,
                     STALE_S, ZONES)
from .registry import Mode, Zone

if TYPE_CHECKING:
    from .commands import CommandManager
    from .db import Store
    from .ml_runtime import ModelRuntime
    from .registry import ZoneRegistry
    from .ws import WsHub

log = logging.getLogger(__name__)

FLAG_ADC_ERR = 0x02


def threshold_dur(zone: Zone) -> int:
    c = zone.config
    if c.collect_mode:
        lo, hi = COLLECT_DUR_RANGE
        return min(random.randint(lo, hi), c.pump_max_s)
    return c.threshold_dur


def in_cooldown(zone: Zone, now: datetime) -> bool:
    end = zone.state.last_water_end
    return end is not None and (now - end).total_seconds() < zone.config.cooldown_s


class DecisionEngine:
    def __init__(self, store: "Store", registry: "ZoneRegistry", commands: "CommandManager",
                 ml: "ModelRuntime", ws: "WsHub") -> None:
        self.store = store
        self.registry = registry
        self.commands = commands
        self.ml = ml
        self.ws = ws

    async def run(self) -> None:
        while True:
            await asyncio.sleep(DECISION_PERIOD_S - time.time() % DECISION_PERIOD_S)
            for i, zone_id in enumerate(ZONES):
                if i:
                    await asyncio.sleep(DECISION_STAGGER_S)
                try:
                    await self.decide_zone(zone_id)
                except Exception:
                    log.exception("Quyết định Zone %d lỗi", zone_id)

    async def decide_zone(self, zone_id: int, now: datetime | None = None) -> dict:
        now = now or datetime.now(timezone.utc)
        zone = self.registry.zones[zone_id]
        cfg, st = zone.config, zone.state
        doc = {"ts": now, "zone_id": zone_id, "mode": cfg.mode.value, "features": None, "p_need": None,
               "need": None, "dur_pred": None, "dur": None, "action_taken": None, "cmd_seq": None,
               "model_version": self.ml.version if cfg.mode is Mode.AI else None}

        doc["action_taken"] = await self._decide(zone, now, doc)
        await self.store.insert_prediction(doc)
        st.last_prediction = {k: v for k, v in doc.items() if k != "features"}
        await self.ws.broadcast("prediction", zone_id, st.last_prediction)
        log.info("Zone %d %s: %s (soil=%s p_need=%s dur=%s)", zone_id, cfg.mode.value, doc["action_taken"],
                 (st.tel or {}).get("soil"), doc["p_need"], doc["dur"])
        return doc

    async def _decide(self, zone: Zone, now: datetime, doc: dict) -> str:
        cfg, st = zone.config, zone.state
        if cfg.mode is Mode.MANUAL:
            return "skip_manual"
        if not st.online:
            return "skip_offline"
        if st.last_seen is None or (now - st.last_seen).total_seconds() > STALE_S or not st.tel:
            return "skip_stale"
        if st.tel.get("flags", 0) & FLAG_ADC_ERR:
            return "skip_sensor"

        if cfg.mode is Mode.THRESHOLD:
            doc["need"] = st.tel["soil"] < cfg.soil_low
            dur = threshold_dur(zone)
        else:
            if not self.ml.available:
                return "skip_no_model"
            x = await self.ml.features(self.store, cfg.id, now)
            if x is None:
                return "skip_stale"
            doc["features"] = x
            doc["p_need"] = self.ml.predict_need(x)
            doc["need"] = doc["p_need"] >= AI_P_THRESHOLD
            doc["dur_pred"] = self.ml.predict_dur(x, cfg.soil_target, cfg.id)
            dur = cfg.threshold_dur if doc["dur_pred"] is None else round(doc["dur_pred"])
            dur = max(AI_DUR_MIN_S, min(dur, cfg.pump_max_s))

        if not doc["need"]:
            return "none"
        doc["dur"] = dur
        if st.pump:
            return "skip_pump_on"
        if in_cooldown(zone, now):
            return "skip_cooldown"
        try:
            cmd = await self.commands.water(cfg.id, dur, decided_by=cfg.mode.value.lower())
        except GatewayOffline:
            return "skip_gw_offline"
        doc["cmd_seq"] = cmd["seq"]
        return "water"
