"""Gán nhãn need_water (H = 2h) và tạo mẫu thời lượng tưới (docs/DESIGN.md §9.2).

Tầng 1, tại mốc t của một Zone: need_water(t) = 1 nếu trong (t, t+H] độ ẩm xuống dưới soil_low
hoặc (use_events=True) có một lần tưới bắt đầu. Loại mẫu: trong 30 phút sau một lần tưới,
thiếu dữ liệu tương lai (khoảng trống, hoặc chưa đủ H).
use_events=False (dataset công khai tưới theo lịch): mẫu có lần tưới trong (t, t+H] bị loại, vì
lần tưới đó che mất việc đất có khô xuống dưới ngưỡng hay không.
"""
from __future__ import annotations

from datetime import timedelta

import numpy as np
import pandas as pd

from . import features as F

H = timedelta(hours=2)
SETTLE = timedelta(minutes=30)
MIN_COVERAGE = 0.8  # tỉ lệ số mẫu tối thiểu trong (t, t+H] so với chu kỳ đo danh nghĩa


def _period(readings: pd.DataFrame) -> pd.Timedelta:
    return readings["ts"].diff().median()


def _utc64(s: pd.Series) -> np.ndarray:
    """Cột datetime có múi giờ -> datetime64[ns] UTC (searchsorted được)."""
    return s.dt.tz_convert("UTC").dt.tz_localize(None).to_numpy("datetime64[ns]")


def default_grid(readings: pd.DataFrame, step: timedelta = timedelta(minutes=5)) -> pd.DatetimeIndex:
    """Lưới mốc quyết định :00/:05 như server (S13)."""
    start, end = readings["ts"].iloc[0].ceil(step), readings["ts"].iloc[-1].floor(step)
    return pd.date_range(start, end, freq=step)


def make_need_samples(readings: pd.DataFrame, events: pd.DataFrame, zone_id: int, soil_low: float,
                      grid=None, use_events: bool = True, horizon: timedelta = H) -> pd.DataFrame:
    """Một dòng mỗi mốc hợp lệ: cột `at`, các cột F.FEATURES, `y`, `soil_below_now`."""
    readings = readings.dropna(subset=["soil"]).reset_index(drop=True)
    if readings.empty:
        return pd.DataFrame(columns=["at", *F.FEATURES, "y", "soil_below_now"])
    grid = default_grid(readings) if grid is None else pd.DatetimeIndex(grid)
    ts = _utc64(readings["ts"])
    soil = readings["soil"].to_numpy()
    need_n = MIN_COVERAGE * (horizon / _period(readings))
    starts = _utc64(events["start_ts"]) if len(events) else np.array([], dtype="datetime64[ns]")
    ends = _utc64(events["start_ts"] + pd.to_timedelta(events["duration_s"], unit="s") + SETTLE) \
        if len(events) else starts
    hist = np.timedelta64(65, "m")
    hz = np.timedelta64(int(horizon.total_seconds()), "s")
    rows = []
    for at in grid:
        a = at.tz_convert("UTC").tz_localize(None).to_datetime64()
        if a + hz > ts[-1]:
            break
        if ((starts <= a) & (a <= ends)).any():          # độ ẩm chưa ổn định sau khi tưới
            continue
        lo, hi = np.searchsorted(ts, a, "right"), np.searchsorted(ts, a + hz, "right")
        if hi - lo < need_n:                              # khoảng trống dữ liệu
            continue
        water_ahead = ((starts > a) & (starts <= a + hz)).any()
        if water_ahead and not use_events:
            continue
        y = int(water_ahead or (soil[lo:hi] < soil_low).any())
        h0 = np.searchsorted(ts, a - hist, "right")
        x = F.build_features(readings.iloc[h0:lo], events, at, zone_id)
        if x is None:
            continue
        rows.append({"at": at, **x, "y": y, "soil_below_now": int(x["soil"] < soil_low)})
    return pd.DataFrame(rows)


def make_duration_samples(readings: pd.DataFrame, events: pd.DataFrame, zone_id: int,
                          after: timedelta = timedelta(minutes=10)) -> pd.DataFrame:
    """Tầng 2: mỗi lần tưới một mẫu (F.DUR_FEATURES + `duration_s`). Chỉ dùng được cho dữ liệu thật."""
    rows = []
    for ev in events.itertuples(index=False):
        t0 = ev.start_ts
        before = F._window_mean(readings, "soil", t0)
        after_v = F._window_mean(readings, "soil", t0 + after)
        temp, hum = F._window_mean(readings, "temp", t0), F._window_mean(readings, "hum", t0)
        if np.isnan(before) or np.isnan(after_v):
            continue
        rows.append({**F.build_duration_features(before, after_v - before, temp, hum, zone_id),
                     "duration_s": float(ev.duration_s)})
    return pd.DataFrame(rows)
