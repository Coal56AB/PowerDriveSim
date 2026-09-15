"""Render README figures from actual CLI results. Run from the repository root."""
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

def load(name):
    with open("build/" + name + ".csv", encoding="utf-8") as f:
        rows = csv.reader(line for line in f if not line.startswith("#"))
        header = next(rows)
        data = np.array([[float(v) for v in row] for row in rows])
    column = next(i for i, h in enumerate(header) if h.startswith("u:output["))
    return data[:, 0], data[:, column]

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                     "axes.spines.top": False, "axes.spines.right": False})
fig, axes = plt.subplots(1, 3, figsize=(15.5, 5.3), facecolor="#f5f7fb")
fig.subplots_adjust(left=.065, right=.97, top=.72, bottom=.22, wspace=.32)
fig.text(.06, .91, "PowerDriveSim", fontsize=27, weight="bold", color="#182a3b")
fig.text(.06, .83, "Результаты расчётов численного ядра", fontsize=14, color="#526575")
for ax in axes:
    ax.set_facecolor("#f5f7fb")
    ax.grid(alpha=.15)
    ax.set_xlabel("Время, мс", labelpad=8)
    ax.set_ylabel("Напряжение, В", labelpad=8)
    ax.spines["bottom"].set_color("#bcc7d3")
    ax.spines["left"].set_color("#bcc7d3")
    ax.tick_params(colors="#526575")

t, u = load("rc-trapezoidal")
axes[0].plot(t*1000, 1-np.exp(-t/.001), "--", color="#91a1b2", lw=3, label="Аналитика")
axes[0].plot(t*1000, u, color="#126bcb", lw=1.7, label="Расчёт")
axes[0].set_title("Заряд RC-цепи", loc="left", weight="bold", pad=16)
axes[0].legend(frameon=False, fontsize=9, loc="lower right")
axes[0].set_xlim(0, 5)

t, u = load("rlc")
exact = 1-np.exp(-100*t)*(np.cos(300*t)+np.sin(300*t)/3)
axes[1].plot(t*1000, exact, "--", color="#91a1b2", lw=3)
axes[1].plot(t*1000, u, color="#087e70", lw=1.7)
axes[1].set_title("Переходный процесс RLC", loc="left", weight="bold", pad=16)
axes[1].set_xlim(0, 40)

t, u = load("switch")
axes[2].step(t*1000, u, where="post", color="#bd6d10", lw=2)
axes[2].axvline(4.3, color="#bd6d10", alpha=.25, ls="--")
axes[2].axvline(8.1, color="#bd6d10", alpha=.25, ls="--")
axes[2].set_title("Переключение идеальных ключей", loc="left", weight="bold", pad=16, fontsize=11)
axes[2].set_xlim(0, 10)
axes[2].set_ylim(-.5, 11)
fig.text(.06, .08, "RC: Trapezoidal, шаг 10 мкс    ·    RLC: Backward Euler, шаг 2 мкс    ·    Ключи: фронты 4,3 и 8,1 мс",
         color="#526575", fontsize=10)
Path("docs/images").mkdir(exist_ok=True)
fig.savefig("docs/images/reference-examples.png", dpi=150, facecolor=fig.get_facecolor())
