from datetime import timedelta

import pytest

from app import ml_runtime  # noqa: F401  (thêm thư mục gốc repo vào sys.path)
from conftest import T0
from ml import features as F


def test_build_features_trends_and_history():
    rows = [{"ts": T0 + timedelta(minutes=m), "soil": 50 - m * 0.1, "temp": 28.0, "hum": 70.0}
            for m in range(0, 66)]
    events = [{"start_ts": T0 + timedelta(minutes=5), "duration_s": 12}]
    at = T0 + timedelta(minutes=65)
    x = F.build_features(F.readings_from_records(rows), F.events_from_records(events), at, 2)
    assert list(x) == F.FEATURES
    assert x["dsoil_60m"] == pytest.approx(-6.0)   # -0.1 %/phút = -6 %/giờ
    assert x["dsoil_30m"] == pytest.approx(-6.0)
    assert x["mins_since_last_water"] == 60 and x["last_water_dur"] == 12 and x["waters_last_24h"] == 1
    assert (x["zone_1"], x["zone_2"], x["zone_3"]) == (0, 1, 0)


def test_build_features_needs_recent_data():
    rows = [{"ts": T0, "soil": 50, "temp": 28, "hum": 70}]
    assert F.build_features(F.readings_from_records(rows), F.events_from_records([]),
                            T0 + timedelta(minutes=10), 1) is None
