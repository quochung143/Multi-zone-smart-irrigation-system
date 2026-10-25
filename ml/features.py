"""build_features(): dùng chung cho train và server runtime (docs/DESIGN.md §9.3).

Mọi nguồn dữ liệu (MongoDB, dataset công khai) đi qua một adapter để ra cùng hai DataFrame chuẩn:
- readings: cột ts (datetime có múi giờ), soil, temp, hum
- events:   cột start_ts, duration_s
rồi mới gọi build_features() / build_duration_features().
"""
from __future__ import annotations

import math
from datetime import datetime, timedelta

import pandas as pd

ZONES = (1, 2, 3)
CURRENT_WINDOW = timedelta(minutes=5)
NO_WATER_MINS = 24 * 60  # chưa tưới lần nào trong lịch sử: coi như 24 giờ

FEATURES = [
    "soil", "temp", "hum",
    "dsoil_30m", "dsoil_60m", "dtemp_60m",
    "hour_sin", "hour_cos",
    "mins_since_last_water", "last_water_dur", "waters_last_24h",
    *(f"zone_{z}" for z in ZONES),
]
DUR_FEATURES = ["soil_before", "delta_target", "temp", "hum", *(f"zone_{z}" for z in ZONES)]


def readings_from_records(records: list[dict]) -> pd.DataFrame:
    """Adapter MongoDB: tài liệu `readings` -> DataFrame chuẩn."""
    df = pd.DataFrame(records, columns=["ts", "soil", "temp", "hum"])
    if not df.empty:
        df["ts"] = pd.to_datetime(df["ts"], utc=True)
    return df.sort_values("ts").reset_index(drop=True)


def events_from_records(records: list[dict]) -> pd.DataFrame:
    """Adapter MongoDB: tài liệu `irrigation_events` -> DataFrame chuẩn."""
    df = pd.DataFrame(records, columns=["start_ts", "duration_s"])
    if not df.empty:
        df["start_ts"] = pd.to_datetime(df["start_ts"], utc=True)
    return df.sort_values("start_ts").reset_index(drop=True)


def _window_mean(df: pd.DataFrame, col: str, end: pd.Timestamp) -> float:
    w = df[(df["ts"] > end - CURRENT_WINDOW) & (df["ts"] <= end)][col].dropna()
    return float(w.mean()) if len(w) else math.nan


def _slope_per_hour(df: pd.DataFrame, col: str, at: pd.Timestamp, back: timedelta) -> float:
    now, before = _window_mean(df, col, at), _window_mean(df, col, at - back)
    return (now - before) / (back.total_seconds() / 3600)


def _one_hot(zone_id: int) -> dict:
    return {f"zone_{z}": int(z == zone_id) for z in ZONES}


def build_features(readings: pd.DataFrame, events: pd.DataFrame, at: datetime, zone_id: int) -> dict | None:
    """Vector đặc trưng tầng 1 tại thời điểm `at`. None nếu không có dữ liệu 5 phút gần nhất."""
    at = pd.Timestamp(at)
    soil = _window_mean(readings, "soil", at) if len(readings) else math.nan
    if math.isnan(soil):
        return None
    local = at.tz_convert(datetime.now().astimezone().tzinfo)
    hour = local.hour + local.minute / 60
    past = events[events["start_ts"] <= at] if len(events) else events
    last = past.iloc[-1] if len(past) else None
    return {
        "soil": soil,
        "temp": _window_mean(readings, "temp", at),
        "hum": _window_mean(readings, "hum", at),
        "dsoil_30m": _slope_per_hour(readings, "soil", at, timedelta(minutes=30)),
        "dsoil_60m": _slope_per_hour(readings, "soil", at, timedelta(minutes=60)),
        "dtemp_60m": _slope_per_hour(readings, "temp", at, timedelta(minutes=60)),
        "hour_sin": math.sin(2 * math.pi * hour / 24),
        "hour_cos": math.cos(2 * math.pi * hour / 24),
        "mins_since_last_water": (at - last["start_ts"]).total_seconds() / 60 if last is not None else NO_WATER_MINS,
        "last_water_dur": float(last["duration_s"]) if last is not None else 0.0,
        "waters_last_24h": int((past["start_ts"] > at - timedelta(hours=24)).sum()) if len(past) else 0,
        **_one_hot(zone_id),
    }


def build_duration_features(soil_before: float, delta_target: float, temp: float, hum: float, zone_id: int) -> dict:
    """Vector đặc trưng tầng 2. Train: delta = soil_after_10m - soil_before; runtime: soil_target - soil_now."""
    return {"soil_before": soil_before, "delta_target": delta_target, "temp": temp, "hum": hum,
            **_one_hot(zone_id)}
