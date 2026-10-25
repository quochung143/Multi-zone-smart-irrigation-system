"""REST API cho dashboard (DESIGN §10.1)."""
from __future__ import annotations

import statistics
from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, HTTPException, Query, Request
from pydantic import BaseModel, Field, ValidationError

from . import auth
from .commands import GatewayOffline
from .registry import Mode

router = APIRouter(prefix="/api")

SOIL_BAND_HIGH = 75.0  # vùng độ ẩm tốt [soil_low, 75] cho §9.5


class ZoneUpdate(BaseModel):
    name: str | None = None
    mode: Mode | None = None
    soil_low: float | None = None
    soil_target: float | None = None
    pump_max_s: int | None = None
    cooldown_s: int | None = None
    threshold_dur: int | None = None
    fallback_s: int | None = None
    collect_mode: bool | None = None
    pump_flow_ml_s: float | None = None


class WaterRequest(BaseModel):
    dur: int = Field(ge=1)


def _svc(request: Request):
    return request.app.state.svc


def _zone(svc, zone_id: int):
    zone = svc.registry.get(zone_id)
    if zone is None:
        raise HTTPException(404, f"không có zone {zone_id}")
    return zone


def _range(frm: datetime | None, to: datetime | None, default: timedelta) -> tuple[datetime, datetime]:
    to = to or datetime.now(timezone.utc)
    frm = frm or to - default
    if to.tzinfo is None:
        to = to.replace(tzinfo=timezone.utc)
    if frm.tzinfo is None:
        frm = frm.replace(tzinfo=timezone.utc)
    return frm, to


def auto_bucket(frm: datetime, to: datetime) -> int | None:
    """6 giờ: thô; tới 24 giờ: 5 phút; dài hơn: 30 phút."""
    span = to - frm
    if span <= timedelta(hours=6):
        return None
    return 300 if span <= timedelta(hours=24) else 1800


@router.get("/health")
async def health(request: Request):
    svc = _svc(request)
    return {"mqtt": svc.link.connected, "gateway": svc.registry.gw_online, "model": svc.ml.version,
            "auth": auth.enabled(request.app)}


@router.get("/zones")
async def zones(request: Request):
    return _svc(request).snapshot()


@router.put("/zones/{zone_id}")
async def update_zone(zone_id: int, body: ZoneUpdate, request: Request):
    svc = _svc(request)
    _zone(svc, zone_id)
    changes = body.model_dump(exclude_none=True)
    if changes.get("mode") is Mode.AI and not svc.ml.available:
        raise HTTPException(409, "chưa có model AI (ml/models/meta.json)")
    try:
        await svc.registry.update_config(zone_id, changes)
    except ValidationError as e:
        raise HTTPException(422, [err["msg"] for err in e.errors()]) from None
    cfg_sent = False
    if svc.registry.gw_online:
        try:
            await svc.commands.cfg(zone_id)
            cfg_sent = True
        except GatewayOffline:
            pass
    snap = svc.registry.snapshot(zone_id)
    await svc.ws.broadcast("zone", zone_id, snap)
    return {**snap, "cfg_sent": cfg_sent}


@router.post("/zones/{zone_id}/water")
async def water(zone_id: int, body: WaterRequest, request: Request):
    svc = _svc(request)
    zone = _zone(svc, zone_id)
    if body.dur > zone.config.pump_max_s:
        raise HTTPException(422, f"dur phải trong [1, {zone.config.pump_max_s}]")
    if not zone.state.online:
        raise HTTPException(409, "zone offline")
    try:
        return await svc.commands.water(zone_id, body.dur, decided_by="manual", force=True)
    except GatewayOffline as e:
        raise HTTPException(409, str(e)) from None


@router.post("/zones/{zone_id}/stop")
async def stop(zone_id: int, request: Request):
    svc = _svc(request)
    _zone(svc, zone_id)
    try:
        return await svc.commands.stop(zone_id)
    except GatewayOffline as e:
        raise HTTPException(409, str(e)) from None


@router.get("/readings")
async def readings(request: Request, zone: int | None = None, frm: datetime | None = Query(None, alias="from"),
                   to: datetime | None = None, bucket: int | None = Query(None, ge=0)):
    frm, to = _range(frm, to, timedelta(hours=6))
    bucket_s = auto_bucket(frm, to) if bucket is None else (bucket or None)
    rows = await _svc(request).store.readings(zone, frm, to, bucket_s)
    return {"bucket_s": bucket_s, "rows": rows}


@router.get("/events")
async def events(request: Request, zone: int | None = None, frm: datetime | None = Query(None, alias="from"),
                 to: datetime | None = None):
    frm, to = _range(frm, to, timedelta(hours=24))
    return await _svc(request).store.events(zone, frm, to)


@router.get("/predictions")
async def predictions(request: Request, zone: int | None = None,
                      frm: datetime | None = Query(None, alias="from"), to: datetime | None = None):
    frm, to = _range(frm, to, timedelta(hours=24))
    return await _svc(request).store.predictions(zone, frm, to)


@router.get("/commands")
async def commands(request: Request, limit: int = Query(50, ge=1, le=500)):
    return await _svc(request).store.commands(limit)


@router.get("/stats")
async def stats(request: Request, zone: int | None = None, frm: datetime | None = Query(None, alias="from"),
                to: datetime | None = None):
    """Chỉ số so sánh AI và ngưỡng (DESIGN §9.5)."""
    svc = _svc(request)
    frm, to = _range(frm, to, timedelta(hours=24))
    out = []
    for zone_id in ([zone] if zone else list(svc.registry.zones)):
        cfg = _zone(svc, zone_id).config
        soils = [r["soil"] for r in await svc.store.readings(zone_id, frm, to, None) if r.get("soil") is not None]
        evs = [e for e in await svc.store.events(zone_id, frm, to) if e.get("duration_s")]
        total_s = sum(e["duration_s"] for e in evs)
        out.append({
            "zone_id": zone_id, "mode": cfg.mode.value, "waterings": len(evs), "water_s": total_s,
            "water_ml": total_s * cfg.pump_flow_ml_s if cfg.pump_flow_ml_s else None,
            "soil_mean": statistics.fmean(soils) if soils else None,
            "soil_std": statistics.pstdev(soils) if len(soils) > 1 else None,
            "pct_in_band": 100 * sum(cfg.soil_low <= s <= SOIL_BAND_HIGH for s in soils) / len(soils) if soils else None,
            "readings": len(soils),
        })
    return out
