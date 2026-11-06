from datetime import timedelta

import pandas as pd

from app import ml_runtime  # noqa: F401  (thêm thư mục gốc repo vào sys.path)
from conftest import T0
from ml import features as F
from ml import label as L


def _readings(soil_fn, minutes=300, step=5):
    return F.readings_from_records([{"ts": T0 + timedelta(minutes=m), "soil": soil_fn(m), "temp": 28.0,
                                     "hum": 70.0} for m in range(0, minutes + 1, step)])


def test_need_label_looks_two_hours_ahead():
    r = _readings(lambda m: 50 - m * 0.1)          # 50% -> 20%, chạm 35% ở phút 150
    s = L.make_need_samples(r, F.events_from_records([]), 1, soil_low=35, grid=r["ts"])
    y = dict(zip(s["at"], s["y"]))
    assert y[T0 + timedelta(minutes=25)] == 0       # 35% sớm nhất ở phút 150 > 25 + 120
    assert y[T0 + timedelta(minutes=35)] == 1       # dự đoán trước khi chạm ngưỡng
    assert s["at"].max() <= r["ts"].max() - L.H     # không đủ cửa sổ tương lai thì bỏ


def test_need_label_events_and_settle():
    r = _readings(lambda m: 60.0)
    ev = F.events_from_records([{"start_ts": T0 + timedelta(minutes=200), "duration_s": 10}])
    s = L.make_need_samples(r, ev, 1, soil_low=35, grid=r["ts"])
    at = set(s["at"])
    assert T0 + timedelta(minutes=220) not in at    # trong 30 phút sau khi tưới
    assert s.set_index("at").loc[T0 + timedelta(minutes=100), "y"] == 1   # có tưới trong 2h tới
    s2 = L.make_need_samples(r, ev, 1, soil_low=35, grid=r["ts"], use_events=False)
    assert T0 + timedelta(minutes=100) not in set(s2["at"]) and s2["y"].sum() == 0


def test_duration_samples():
    r = _readings(lambda m: 40.0 if m <= 100 else 48.0)
    ev = F.events_from_records([{"start_ts": T0 + timedelta(minutes=100), "duration_s": 12}])
    d = L.make_duration_samples(r, ev, 2)
    assert list(d.columns) == [*F.DUR_FEATURES, "duration_s"]
    assert d.iloc[0]["soil_before"] == 40.0 and d.iloc[0]["delta_target"] == 8.0
    assert isinstance(d, pd.DataFrame) and d.iloc[0]["zone_2"] == 1
