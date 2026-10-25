"""Nạp model từ ml/models (meta.json + *.joblib) và dự đoán p_need, dur (DESIGN §9).

meta.json: {"version": "v1", "clf": "clf_v1.joblib", "reg": "reg_v1.joblib",
            "features": [...], "dur_features": [...]}   ("reg" có thể thiếu: dùng threshold_dur)
"""
from __future__ import annotations

import json
import logging
import sys
from datetime import datetime, timedelta
from pathlib import Path
from typing import TYPE_CHECKING

from .config import REPO_DIR

if str(REPO_DIR) not in sys.path:
    sys.path.insert(0, str(REPO_DIR))

from ml import features as F  # noqa: E402  (dùng chung với lúc train)

if TYPE_CHECKING:
    from .db import Store

log = logging.getLogger(__name__)

HISTORY = timedelta(minutes=65)       # đủ cho dsoil_60m + cửa sổ 5 phút
WATER_HISTORY = timedelta(hours=24)   # waters_last_24h


class ModelRuntime:
    def __init__(self, models_dir: Path) -> None:
        self.models_dir = Path(models_dir)
        self.meta: dict | None = None
        self.clf = self.reg = None
        self.load()

    @property
    def available(self) -> bool:
        return self.clf is not None

    @property
    def version(self) -> str | None:
        return self.meta.get("version") if self.meta else None

    def load(self) -> None:
        meta_path = self.models_dir / "meta.json"
        if not meta_path.exists():
            log.info("Chưa có model (%s): chế độ AI bị khoá", meta_path)
            return
        try:
            import joblib

            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            clf = joblib.load(self.models_dir / meta["clf"])
            reg = joblib.load(self.models_dir / meta["reg"]) if meta.get("reg") else None
        except Exception:
            log.exception("Nạp model lỗi: chế độ AI bị khoá")
            return
        self.meta, self.clf, self.reg = meta, clf, reg
        log.info("Đã nạp model %s", self.version)

    async def features(self, store: "Store", zone_id: int, at: datetime) -> dict | None:
        readings = await store.readings(zone_id, at - HISTORY, at, None)
        events = await store.events(zone_id, at - WATER_HISTORY, at)
        return F.build_features(F.readings_from_records(readings), F.events_from_records(events), at, zone_id)

    def predict_need(self, x: dict) -> float:
        import pandas as pd

        cols = self.meta.get("features", F.FEATURES)
        return float(self.clf.predict_proba(pd.DataFrame([x])[cols])[0][1])

    def predict_dur(self, x: dict, soil_target: float, zone_id: int) -> float | None:
        if self.reg is None:
            return None
        import pandas as pd

        dx = F.build_duration_features(x["soil"], soil_target - x["soil"], x["temp"], x["hum"], zone_id)
        cols = self.meta.get("dur_features", F.DUR_FEATURES)
        return float(self.reg.predict(pd.DataFrame([dx])[cols])[0])
