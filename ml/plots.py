"""Biểu đồ so sánh mô hình tầng 1 cho báo cáo -> ml/reports/figures/*.png (+ metrics_test.csv).

    python -m ml.plots [--data ml/data/raw/mendeley_jtsyb593fy.xlsx]

Chạy lại đúng thí nghiệm của ml/train.py (cùng seed, cùng chia 70/15/15) nên số liệu khớp meta.json.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib import font_manager  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402
from sklearn.metrics import average_precision_score, precision_recall_curve, roc_auc_score, roc_curve  # noqa: E402

from . import features as F  # noqa: E402
from . import train as T  # noqa: E402

OUT = T.ML_DIR / "reports" / "figures"

# Bảng màu tham chiếu (dataviz palette.md, đã chạy validate_palette.js: 3 slot đầu pass all-pairs)
SURFACE, INK, INK2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
COLORS = {"rf": "#2a78d6", "xgb": "#eb6834", "mlp": "#1baf7a", "baseline": "#a8a69e"}
NAMES = {"rf": "Random Forest", "xgb": "XGBoost", "mlp": "MLP (mạng nơ-ron)", "baseline": "Baseline ngưỡng"}
BLUES = LinearSegmentedColormap.from_list("blues", ["#f3f7fd", "#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"])
MODELS = ["rf", "xgb", "mlp"]


def _style() -> None:
    names = {f.name for f in font_manager.fontManager.ttflist}
    font = next((f for f in ("Segoe UI", "Arial", "DejaVu Sans") if f in names), "DejaVu Sans")
    plt.rcParams.update({
        "font.family": font, "font.size": 10, "figure.dpi": 110, "savefig.dpi": 200,
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "axes.titlecolor": INK, "axes.titlesize": 11,
        "axes.titleweight": "semibold", "axes.titlelocation": "left", "axes.spines.top": False,
        "axes.spines.right": False, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
        "axes.axisbelow": True, "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2,
        "ytick.labelcolor": INK2, "legend.frameon": False, "legend.labelcolor": INK2, "lines.linewidth": 2,
    })


def _save(fig, name: str) -> Path:
    path = OUT / name
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print("  ", path.relative_to(T.ML_DIR.parent))
    return path


def _proba(ex, name, part="test"):
    return ex["best"][name]["model"].predict_proba(ex[part][F.FEATURES])[:, 1]


# --- 1. Chỉ số trên tập Test --------------------------------------------------------------------
def fig_metrics(ex) -> None:
    keys = [("accuracy", "Accuracy"), ("precision", "Precision"), ("recall", "Recall"), ("f1", "F1"),
            ("early_recall", "Bắt sớm*")]
    series = {m: ex["best"][m]["test"] for m in MODELS} | {"baseline": ex["baseline"]["test"]}
    fig, ax = plt.subplots(figsize=(9, 4.2))
    n, w = len(series), 0.19
    x = np.arange(len(keys))
    for i, (m, r) in enumerate(series.items()):
        vals = [r[k] or 0.0 for k, _ in keys]
        pos = x + (i - (n - 1) / 2) * (w + 0.012)
        ax.bar(pos, vals, w, color=COLORS[m], label=NAMES[m], edgecolor=SURFACE, linewidth=1)
        for xp, v in zip(pos, vals):
            ax.text(xp, v + 0.012, f"{v:.2f}", ha="center", va="bottom", fontsize=7.5, color=INK2)
    ax.set_xticks(x, [lbl for _, lbl in keys])
    ax.set_ylim(0, 1.1)
    ax.set_yticks(np.arange(0, 1.01, 0.2))
    ax.grid(axis="x", visible=False)
    ax.tick_params(axis="x", length=0)
    te = ex["test"]
    ax.set_title(f"Chỉ số trên tập Test ({len(te)} mẫu, {int(te.y.sum())} dương)")
    ax.legend(ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.1), fontsize=9)
    early = series["xgb"]["early_pos"]
    fig.text(0.01, -0.08, f"* Bắt sớm: tỉ lệ dự đoán đúng các mẫu đất CÒN trên ngưỡng nhưng sẽ khô trong 2 giờ tới "
             f"({early} mẫu). Baseline ngưỡng luôn bằng 0.", fontsize=8, color=MUTED)
    _save(fig, "fig1_metrics_test.png")


# --- 2. Ma trận nhầm lẫn ------------------------------------------------------------------------
def fig_confusion(ex) -> None:
    items = [(m, ex["best"][m]["test"]["confusion"]) for m in MODELS] + [("baseline", ex["baseline"]["test"]["confusion"])]
    vmax = max(max(c.values()) for _, c in items)
    fig, axes = plt.subplots(1, 4, figsize=(11, 3.1))
    for ax, (m, c) in zip(axes, items):
        mat = np.array([[c["tn"], c["fp"]], [c["fn"], c["tp"]]])
        ax.imshow(mat, cmap=BLUES, vmin=0, vmax=vmax)
        for (i, j), v in np.ndenumerate(mat):
            ax.text(j, i, str(v), ha="center", va="center", fontsize=12, fontweight="semibold",
                    color="#ffffff" if v > vmax * 0.55 else INK)
        ax.set_xticks([0, 1], ["Không tưới", "Cần tưới"])
        ax.set_yticks([0, 1], ["Không tưới", "Cần tưới"] if ax is axes[0] else ["", ""])
        ax.set_title(NAMES[m], fontsize=10)
        ax.grid(False)
        ax.tick_params(length=0)
        for s in ax.spines.values():
            s.set_visible(False)
        ax.set_xlabel("Dự đoán", fontsize=9)
    axes[0].set_ylabel("Thực tế", fontsize=9)
    fig.suptitle("Ma trận nhầm lẫn trên tập Test", x=0.01, ha="left", fontsize=11, fontweight="semibold", color=INK)
    fig.tight_layout()
    _save(fig, "fig2_confusion.png")


# --- 3. Đường Precision–Recall và ROC -----------------------------------------------------------
def fig_curves(ex) -> None:
    te = ex["test"]
    y = te["y"].to_numpy()
    base = ex["baseline"]["test"]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4.2))
    for m in MODELS:
        p = _proba(ex, m)
        pr, rc, _ = precision_recall_curve(y, p)
        a1.step(rc, pr, where="post", color=COLORS[m], label=f"{NAMES[m]} (AP {average_precision_score(y, p):.3f})")
        fpr, tpr, _ = roc_curve(y, p)
        a2.plot(fpr, tpr, color=COLORS[m], label=f"{NAMES[m]} (AUC {roc_auc_score(y, p):.3f})")
    c = base["confusion"]
    b_fpr = c["fp"] / max(c["fp"] + c["tn"], 1)
    for ax, xy in ((a1, (base["recall"], base["precision"])), (a2, (b_fpr, base["recall"]))):
        ax.plot(*xy, "o", ms=9, color=COLORS["baseline"], markeredgecolor=SURFACE, markeredgewidth=2,
                label=f"{NAMES['baseline']} (1 điểm)")
    a2.plot([0, 1], [0, 1], ls="--", lw=1, color=AXIS)
    a1.set(xlabel="Recall", ylabel="Precision", xlim=(0.5, 1.01), ylim=(0.5, 1.02),
           title="Precision–Recall (Test, phóng to góc trên phải)")
    a2.set(xlabel="Tỉ lệ dương tính giả (FPR)", ylabel="Tỉ lệ dương tính thật (TPR)", xlim=(-0.005, 0.4),
           ylim=(0.5, 1.02), title="ROC (Test, phóng to góc trên trái)")
    a1.legend(loc="lower left", fontsize=8.5)
    a2.legend(loc="lower right", fontsize=8.5)
    fig.tight_layout()
    _save(fig, "fig3_pr_roc.png")


# --- 4. Mức quan trọng đặc trưng ----------------------------------------------------------------
def _importance(model) -> np.ndarray:
    est = model.steps[-1][1] if hasattr(model, "steps") else model
    return np.asarray(est.feature_importances_, dtype=float)


def fig_importance(ex) -> None:
    feats = [f for f in F.FEATURES if not f.startswith("zone_")]  # dataset công khai chỉ 1 khay: zone luôn 0
    idx = [F.FEATURES.index(f) for f in feats]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.4), sharey=True)
    order = None
    for ax, m in zip(axes, ("rf", "xgb")):
        imp = _importance(ex["best"][m]["model"])[idx]
        imp = imp / imp.sum()
        if order is None:
            order = np.argsort(imp)
        ax.barh(np.arange(len(feats)), imp[order], color=COLORS[m], height=0.62)
        for yp, v in enumerate(imp[order]):
            ax.text(v + 0.005, yp, f"{v:.0%}", va="center", fontsize=8, color=INK2)
        ax.set_yticks(np.arange(len(feats)), [feats[i] for i in order])
        ax.set_title(NAMES[m], fontsize=10)
        ax.grid(axis="y", visible=False)
        ax.tick_params(axis="y", length=0)
        ax.set_xlim(0, imp.max() * 1.18)
        ax.xaxis.set_major_formatter(matplotlib.ticker.PercentFormatter(1, decimals=0))
    fig.suptitle("Mức quan trọng đặc trưng (chuẩn hoá tổng = 100%)", x=0.01, ha="left",
                 fontsize=11, fontweight="semibold", color=INK)
    fig.tight_layout()
    _save(fig, "fig4_feature_importance.png")


# --- 5. Diễn biến trên tập Test: độ ẩm và xác suất cần tưới -------------------------------------
def fig_timeline(ex) -> None:
    te = ex["test"].copy().reset_index(drop=True)
    t = te["at"].dt.tz_convert(F.LOCAL_TZ)
    gap = t.diff() > pd.Timedelta(minutes=31)   # mẫu bị loại khi gán nhãn (sau tưới): ngắt đường

    def broken(v):
        v = pd.Series(np.asarray(v, dtype=float))
        tt = t.copy()
        for i in np.flatnonzero(gap)[::-1]:     # chèn NaN để matplotlib không nối qua khoảng trống
            v = pd.concat([v.iloc[:i], pd.Series([np.nan]), v.iloc[i:]], ignore_index=True)
            tt = pd.concat([tt.iloc[:i], pd.Series([tt.iloc[i] - pd.Timedelta(minutes=1)]), tt.iloc[i:]],
                           ignore_index=True)
        return tt, v

    lbl = {"fontsize": 8, "color": MUTED, "ha": "right", "va": "bottom",
           "bbox": {"facecolor": SURFACE, "edgecolor": "none", "pad": 1.5}}
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(11, 5.6), sharex=True, gridspec_kw={"height_ratios": [1.1, 1]})

    def shade(ax):
        on = te["y"].to_numpy() == 1
        for i in np.flatnonzero(on):
            ax.axvspan(t.iloc[i] - pd.Timedelta(minutes=15), t.iloc[i] + pd.Timedelta(minutes=15),
                       color="#cde2fb", lw=0, zorder=0)

    shade(a1)
    a1.plot(*broken(te["soil"]), color=INK2, lw=1.6, marker="o", ms=2.5)
    a1.axhline(F.PUBLIC_SOIL_LOW, ls="--", lw=1.2, color=MUTED)
    a1.text(t.iloc[-1] + pd.Timedelta(hours=3), F.PUBLIC_SOIL_LOW + 0.5, f"soil_low = {F.PUBLIC_SOIL_LOW:.0f}%", **lbl)
    a1.set_ylabel("Độ ẩm đất (%)")
    a1.set_title("Tập Test: độ ẩm đất (nền xanh = nhãn 'cần tưới trong 2 giờ tới')")

    shade(a2)
    for m in ("xgb", "rf", "mlp"):
        a2.plot(*broken(_proba(ex, m)), color=COLORS[m], lw=1.6 if m == "xgb" else 1.1,
                alpha=1 if m == "xgb" else 0.85, label=NAMES[m])
    a2.axhline(0.5, ls="--", lw=1.2, color=MUTED)
    a2.text(t.iloc[-1] + pd.Timedelta(hours=3), 0.52, "ngưỡng quyết định 0.5", **lbl)
    a2.set(ylabel="p_need", ylim=(-0.03, 1.05))
    a2.set_title("Xác suất cần tưới do mô hình dự đoán")
    a2.legend(ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.18), fontsize=9)
    a2.xaxis.set_major_formatter(matplotlib.dates.DateFormatter("%d/%m\n%H:%M", tz=F.LOCAL_TZ))
    fig.tight_layout()
    fig.text(0.01, -0.01, "Khoảng trống chiều 19/04: mẫu bị loại khi gán nhãn (≤ 30 phút sau tưới, "
             "hoặc có tưới trong 2 giờ tới).", fontsize=8, color=MUTED)
    _save(fig, "fig5_timeline_test.png")


# --- 6. Độ nhạy siêu tham số (F1 trên Validation) -----------------------------------------------
def fig_tuning(ex) -> None:
    fig, ax = plt.subplots(figsize=(7.5, 3.6))
    rng = np.random.default_rng(0)
    for i, m in enumerate(MODELS):
        f1 = np.array(ex["best"][m]["grid_f1"])
        jitter = rng.uniform(-0.12, 0.12, len(f1))
        ax.scatter(np.full(len(f1), i) + jitter, f1, s=42, color=COLORS[m], edgecolor=SURFACE, linewidth=1.5,
                   zorder=3)
        ax.hlines(f1.max(), i - 0.25, i + 0.25, color=INK, lw=1.5, zorder=4)
        ax.text(i + 0.28, f1.max(), f"tốt nhất {f1.max():.3f}", va="center", fontsize=8, color=INK2)
    b = ex["baseline"]["val"]["f1"]
    ax.axhline(b, ls="--", lw=1.2, color=MUTED)
    ax.text(len(MODELS) - 0.15, b + 0.004, f"baseline ngưỡng {b:.3f}", fontsize=8, color=MUTED, va="bottom",
            ha="right")
    ax.set_ylim(b - 0.03, None)
    ax.set_xticks(range(len(MODELS)), [f"{NAMES[m]}\n({len(ex['best'][m]['grid_f1'])} cấu hình)" for m in MODELS])
    ax.set_xlim(-0.5, len(MODELS) - 0.1)
    ax.grid(axis="x", visible=False)
    ax.tick_params(axis="x", length=0)
    ax.set_ylabel("F1 (Validation)")
    ax.set_title("Độ nhạy theo siêu tham số: F1 của từng cấu hình trong lưới tìm kiếm")
    _save(fig, "fig6_tuning_val.png")


def table(ex) -> None:
    rows = [{"model": NAMES[m], **{k: r[k] for k in ("accuracy", "precision", "recall", "f1", "early_recall")},
             "auc": round(roc_auc_score(ex["test"]["y"], _proba(ex, m)), 4) if m != "baseline" else None}
            for m, r in [*((m, ex["best"][m]["test"]) for m in MODELS), ("baseline", ex["baseline"]["test"])]]
    path = OUT / "metrics_test.csv"
    pd.DataFrame(rows).to_csv(path, index=False, encoding="utf-8-sig")
    print("  ", path.relative_to(T.ML_DIR.parent))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=T.DEFAULT_DATA)
    a = ap.parse_args()
    _style()
    OUT.mkdir(parents=True, exist_ok=True)
    ex = T.experiment(T.load_public(a.data), verbose=False)
    print(f"Model chọn: {ex['chosen']}. Đã ghi:")
    for f in (fig_metrics, fig_confusion, fig_curves, fig_importance, fig_timeline, fig_tuning, table):
        f(ex)


if __name__ == "__main__":
    main()
