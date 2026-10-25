"""ZoneRegistry: cấu hình (gốc ở collection `zones`, ghi xuống trước rồi mới cập nhật RAM) và trạng thái runtime."""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import TYPE_CHECKING

from pydantic import BaseModel, Field, model_validator

from .config import ZONES

if TYPE_CHECKING:
    from .db import Store


class Mode(str, Enum):
    MANUAL = "MANUAL"
    THRESHOLD = "THRESHOLD"
    AI = "AI"


class ZoneConfig(BaseModel):
    id: int
    name: str
    mode: Mode = Mode.THRESHOLD
    soil_low: float = Field(35.0, ge=10, le=80)
    soil_target: float = 60.0
    pump_max_s: int = Field(30, ge=1, le=30)    # nút luôn tự giới hạn 30 s
    cooldown_s: int = Field(600, ge=0)
    threshold_dur: int = Field(10, ge=3)
    fallback_s: int = Field(10, ge=1)           # thời lượng nút tự tưới khi FALLBACK
    collect_mode: bool = True
    pump_flow_ml_s: float = Field(0.0, ge=0)    # đo bằng cốc đong, dùng cho /api/stats

    @model_validator(mode="after")
    def _check(self) -> "ZoneConfig":
        if self.soil_target < self.soil_low + 5:
            raise ValueError("soil_target phải >= soil_low + 5")
        if self.threshold_dur > self.pump_max_s:
            raise ValueError("threshold_dur phải <= pump_max_s")
        if self.fallback_s > self.pump_max_s:
            raise ValueError("fallback_s phải <= pump_max_s")
        return self


@dataclass
class ZoneState:
    online: bool = False
    last_seen: datetime | None = None
    tel: dict | None = None            # bản tin tel gần nhất (kèm ts server)
    pump: bool = False
    last_water_end: datetime | None = None
    last_prediction: dict | None = None
    link_stat: dict | None = None


@dataclass
class Zone:
    config: ZoneConfig
    state: ZoneState = field(default_factory=ZoneState)


class ZoneRegistry:
    def __init__(self, store: "Store") -> None:
        self.store = store
        self.zones: dict[int, Zone] = {}
        self.gw_online = False

    async def load(self) -> None:
        docs = {d["_id"]: d for d in await self.store.load_zones()}
        for z in ZONES:
            if z in docs:
                cfg = ZoneConfig(id=z, **{k: v for k, v in docs[z].items() if k in ZoneConfig.model_fields})
            else:
                cfg = ZoneConfig(id=z, name=f"Zone {z}")
                await self.store.save_zone(cfg.model_dump(mode="json"))
            state = ZoneState(last_water_end=await self.store.last_water_end(z))
            self.zones[z] = Zone(cfg, state)

    def get(self, zone_id: int) -> Zone | None:
        return self.zones.get(zone_id)

    async def update_config(self, zone_id: int, changes: dict) -> ZoneConfig:
        """Kiểm tra (ValueError nếu sai), ghi Mongo rồi mới đổi RAM."""
        zone = self.zones[zone_id]
        cfg = ZoneConfig(**{**zone.config.model_dump(), **changes, "id": zone_id})
        await self.store.save_zone(cfg.model_dump(mode="json"))
        zone.config = cfg
        return cfg

    def snapshot(self, zone_id: int | None = None) -> list[dict] | dict:
        def one(z: Zone) -> dict:
            s = z.state
            return {**z.config.model_dump(mode="json"), "online": s.online, "last_seen": s.last_seen,
                    "tel": s.tel, "pump": s.pump, "last_water_end": s.last_water_end,
                    "last_prediction": s.last_prediction, "link_stat": s.link_stat}
        if zone_id is not None:
            return one(self.zones[zone_id])
        return [one(z) for z in self.zones.values()]
