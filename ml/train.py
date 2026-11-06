"""Train/eval RF, XGBoost và mạng MLP cho tầng 1, xuất models/*.joblib + meta.json (docs/DESIGN.md §9.4).

    python -m ml.train                         # dataset công khai Mendeley jtsyb593fy
    python -m ml.train --data đường/dẫn.xlsx --version v1

Chia theo thời gian 70/15/15, tuning trên Validation theo F1, train lại model tốt nhất trên
Train+Validation rồi báo cáo trên Test, kèm baseline ngưỡng (need = soil < soil_low tại t).
Tầng 2 (thời lượng) không train được từ dataset công khai (không có giây bơm): meta không có "reg",
server dùng threshold_dur cho tới khi hiệu chuẩn tại chỗ.
"""
from __future__ import annotations

import argparse
import itertools
import json
import platform
from datetime import datetime
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
import sklearn
import xgboost
from sklearn.ensemble import RandomForestClassifier
from sklearn.impute import SimpleImputer
from sklearn.metrics import accuracy_score, confusion_matrix, f1_score, precision_score, recall_score
from sklearn.neural_network import MLPClassifier
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from xgboost import XGBClassifier

from . import features as F
from . import label as L

ML_DIR = Path(__file__).resolve().parent
DEFAULT_DATA = ML_DIR / "data" / "raw" / "mendeley_jtsyb593fy.xlsx"
SEED = 42


# --- Ứng viên ------------------------------------------------------------------------------
def _rf(p, pos_w):
    return make_pipeline(SimpleImputer(strategy="median"),
                         RandomForestClassifier(class_weight="balanced", random_state=SEED, n_jobs=-1, **p))


def _xgb(p, pos_w):
    return XGBClassifier(scale_pos_weight=pos_w, eval_metric="logloss", random_state=SEED, n_jobs=-1, **p)


def _mlp(p, pos_w):
    return make_pipeline(SimpleImputer(strategy="median"), StandardScaler(),
                         MLPClassifier(max_iter=3000, random_state=SEED, **p))


CANDIDATES = {
    "rf": (_rf, {"n_estimators": [200, 400], "max_depth": [None, 4, 8], "min_samples_leaf": [1, 3, 5]}),
    "xgb": (_xgb, {"n_estimators": [100, 300], "max_depth": [2, 3, 5], "learning_rate": [0.05, 0.1]}),
    "mlp": (_mlp, {"hidden_layer_sizes": [(32, 16), (64, 32)], "alpha": [1e-3, 1e-2, 1e-1]}),
}
OVERSAMPLE = {"mlp"}  # MLP không có class_weight: nhân bản mẫu dương trong tập train


def _grid(space: dict):
    keys = list(space)
    for vals in itertools.product(*(space[k] for k in keys)):
        yield dict(zip(keys, vals))


def _fit(name, params, X, y):
    build, _ = CANDIDATES[name]
    pos_w = float((y == 0).sum() / max((y == 1).sum(), 1))
    if name in OVERSAMPLE and (y == 1).any():
        rng = np.random.default_rng(SEED)
        pos = np.flatnonzero(y == 1)
        extra = rng.choice(pos, size=max(int((y == 0).sum() - len(pos)), 0), replace=True)
        idx = np.concatenate([np.arange(len(y)), extra])
        X, y = X.iloc[idx], y[idx]
    return build(params, pos_w).fit(X, y)


def _metrics(y, pred, early_mask) -> dict:
    tn, fp, fn, tp = confusion_matrix(y, pred, labels=[0, 1]).ravel()
    early = y.astype(bool) & early_mask  # cần tưới nhưng lúc t độ ẩm còn trên ngưỡng
    return {
        "n": int(len(y)), "pos": int(y.sum()),
        "accuracy": round(accuracy_score(y, pred), 4),
        "precision": round(precision_score(y, pred, zero_division=0), 4),
        "recall": round(recall_score(y, pred, zero_division=0), 4),
        "f1": round(f1_score(y, pred, zero_division=0), 4),
        "confusion": {"tn": int(tn), "fp": int(fp), "fn": int(fn), "tp": int(tp)},
        "early_recall": round(float(pred[early].mean()), 4) if early.any() else None,
        "early_pos": int(early.sum()),
    }


def split(df: pd.DataFrame):
    df = df.sort_values("at").reset_index(drop=True)
    n = len(df)
    a, b = int(n * 0.70), int(n * 0.85)
    return df.iloc[:a], df.iloc[a:b], df.iloc[b:]


def load_public(path: Path) -> pd.DataFrame:
    readings, events = F.from_mendeley_jtsyb593fy(path)
    # Dataset 30 phút/mẫu: mốc quyết định = chính các mốc đo; zone 0 = không thuộc Zone nào (one-hot toàn 0)
    return L.make_need_samples(readings, events, zone_id=0, soil_low=F.PUBLIC_SOIL_LOW,
                               grid=readings["ts"], use_events=False)


def experiment(df: pd.DataFrame, verbose: bool = True) -> dict:
    """Tuning trên Val, train lại trên Train+Val, đo Test. Trả về mọi thứ để báo cáo/vẽ (ml/plots.py)."""
    tr, va, te = split(df)
    cols = F.FEATURES
    if verbose:
        print(f"Mẫu: {len(df)} (dương {int(df.y.sum())}) | train {len(tr)} / val {len(va)} / test {len(te)}")
        print(f"  khoảng thời gian test: {te['at'].iloc[0]} → {te['at'].iloc[-1]}")

    def evaluate(model, part):
        return _metrics(part["y"].to_numpy(), model.predict(part[cols]).astype(int),
                        part["soil_below_now"].to_numpy() == 0)

    def baseline(part):
        return _metrics(part["y"].to_numpy(), part["soil_below_now"].to_numpy(),
                        part["soil_below_now"].to_numpy() == 0)

    # 1) Tuning từng họ model trên Validation
    best = {}
    for name, (_, space) in CANDIDATES.items():
        scored = []
        for p in _grid(space):
            m = _fit(name, p, tr[cols], tr["y"].to_numpy())
            scored.append((evaluate(m, va)["f1"], p))
        f1, p = max(scored, key=lambda s: s[0])  # hoà: giữ cấu hình đầu (đơn giản hơn)
        best[name] = {"params": p, "val": evaluate(_fit(name, p, tr[cols], tr["y"].to_numpy()), va),
                      "grid_f1": [sc for sc, _ in scored]}
        if verbose:
            print(f"  {name:4s} val F1={f1:.3f}  {p}")

    # 2) Train lại trên Train+Val, đo trên Test
    trva = pd.concat([tr, va])
    for name, b in best.items():
        model = _fit(name, b["params"], trva[cols], trva["y"].to_numpy())
        b["test"], b["model"] = evaluate(model, te), model

    chosen = max(best, key=lambda k: (best[k]["val"]["f1"], -list(CANDIDATES).index(k)))
    base = {"val": baseline(va), "test": baseline(te)}
    return {"df": df, "train": tr, "val": va, "test": te, "best": best, "chosen": chosen, "baseline": base}


def run(data: Path, out: Path, version: str) -> dict:
    df = load_public(data)
    ex = experiment(df)
    tr, va, te, best, chosen, base = ex["train"], ex["val"], ex["test"], ex["best"], ex["chosen"], ex["baseline"]
    cols = F.FEATURES

    print("\nTest (train lại trên train+val):")
    print(f"  {'model':9s} {'acc':>6s} {'prec':>6s} {'rec':>6s} {'F1':>6s} {'early_rec':>9s}")
    for name, r in [*((k, best[k]["test"]) for k in best), ("baseline", base["test"])]:
        er = "-" if r["early_recall"] is None else f"{r['early_recall']:.3f}"
        mark = " *" if name == chosen else ""
        print(f"  {name:9s} {r['accuracy']:6.3f} {r['precision']:6.3f} {r['recall']:6.3f} {r['f1']:6.3f} {er:>9s}{mark}")
    print(f"  (early_rec: tỉ lệ bắt được mẫu 'chưa khô nhưng sẽ khô trong 2h' — baseline luôn = 0)")

    out.mkdir(parents=True, exist_ok=True)
    clf_file = f"clf_{version}.joblib"
    joblib.dump(best[chosen]["model"], out / clf_file)
    meta = {
        "version": version, "clf": clf_file, "reg": None, "model": chosen,
        "features": cols, "dur_features": F.DUR_FEATURES,
        "trained_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "source": {"name": "mendeley:jtsyb593fy", "file": data.name, "rows": int(len(df)),
                   "soil_low": F.PUBLIC_SOIL_LOW, "soil_shift": F.MENDELEY_SOIL_LOW - F.PUBLIC_SOIL_LOW,
                   "label": "soil<soil_low trong 2h (use_events=False)"},
        "split": {"train": len(tr), "val": len(va), "test": len(te)},
        "metrics": {k: {"params": v["params"], "val": v["val"], "test": v["test"]} for k, v in best.items()},
        "baseline": base,
        "env": {"python": platform.python_version(), "sklearn": sklearn.__version__,
                "xgboost": xgboost.__version__},
    }
    (out / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    print(f"\nĐã lưu {out / clf_file} và meta.json (model: {chosen})")
    return meta


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=DEFAULT_DATA)
    ap.add_argument("--out", type=Path, default=ML_DIR / "models")
    ap.add_argument("--version", default="v1")
    a = ap.parse_args()
    run(a.data, a.out, a.version)


if __name__ == "__main__":
    main()
