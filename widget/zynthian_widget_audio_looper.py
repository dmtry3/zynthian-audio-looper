#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Audio Looper: виджет в стиле экрана Aeros.
#   Loop:  сверху тонкая полоса темпа; строки дорожек: точка парта, круг позиции с номером,
#          волна во всю ширину; справа индикатор MASTER; снизу три вкладки с подписями.
#   Mixer: на каждую дорожку круг, кнопка M и сдвоенный фейдер; справа MASTER.
# Чёрный фон без плашек, цвет несёт состояние. Виджет только читает выходные порты (self.monitors).
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

# ---------- палитра (цвета состояний из ТЗ, оттенки как у Aeros) ----------
BG = "#000000"
TAB = "#262626"
GRID = "#2e2e2e"
DIM = "#5a5a5a"
TEXT = "#ffffff"
GREEN = "#3ddc4a"      # активная дорожка, текущий парт
BLUE = "#4a86e8"       # играет
RED = "#ff3b30"        # запись, конец цикла
ORANGE = "#ffb000"     # овердаб
GRAY = "#8c8c8c"       # mute
PURPLE = "#a66bff"     # reverse
EMPTY = "#3a3a3a"      # пустая дорожка: тёмный контур
METER_OFF = ("#0b1d0d", "#1f1c07", "#220a09")   # погашенные сегменты: зелёный, жёлтый, красный
METER_ON = ("#3ddc4a", "#ffd23a", "#ff3b30")

# Коды порта o_action, как enum Action в lv2/audio_looper.cpp: (подпись, цвет)
ACTIONS = [
    ("READY", DIM),
    ("REC / DUB", RED),
    ("UNDO", ORANGE),
    ("REDO", ORANGE),
    ("RESET TRACK", GRAY),
    ("RESET ALL", GRAY),
    ("MUTE", GRAY),
    ("REVERSE", PURPLE),
    ("LOCK", ORANGE),
    ("NEXT TRACK", GREEN),
    ("PREV TRACK", GREEN),
    ("NEXT PART", GREEN),
    ("PREV PART", GREEN),
    ("PLAY / STOP", BLUE),
    ("LOOP", TEXT),
    ("MIXER", TEXT),
]
ACTION_HOLD_S = 1.5     # сколько подпись последнего действия горит ярко
FLASH_S = 0.12          # вспышка полосы темпа на сильной доле
FONT_PREFS = ("DejaVu Sans Condensed", "DejaVu Sans", "Liberation Sans Narrow", "Liberation Sans")


class zynthian_widget_audio_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat', bg=BG)
        self.widget_canvas.grid(sticky='news')
        self.family = None
        self.fonts = {}
        self.last_bar = None
        self.flash_until = 0.0
        self.last_cnt = None
        self.action_until = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.refresh_gui()

    # ---------- текст, который всегда влезает в свою клетку ----------
    def _family(self):
        if self.family is None:
            have = set(tkinter.font.families(self))
            self.family = next((f for f in FONT_PREFS if f in have), zynthian_gui_config.font_family)
        return self.family

    def _font(self, px, bold):
        key = (px, bold)
        f = self.fonts.get(key)
        if f is None:
            f = tkinter.font.Font(family=self._family(), size=-px, weight="bold" if bold else "normal")
            self.fonts[key] = f
        return f

    def _fit(self, text, max_w, max_h, bold):
        """Самый крупный шрифт, в котором text влезает в клетку; None, если не влезает совсем."""
        px = max(6, int(max_h))
        while True:
            f = self._font(px, bold)
            if f.measure(text) <= max_w and f.metrics("linespace") <= max_h:
                return f
            if px <= 6:
                return None
            px = max(6, int(px * 0.9))

    def _text(self, x, y, text, max_w, max_h, color, anchor="center", bold=True):
        if not text or max_w < 4 or max_h < 4:
            return
        f = self._fit(text, max_w, max_h, bold)
        if f is not None:
            self.widget_canvas.create_text(x, y, text=text, fill=color, anchor=anchor, font=f)

    # ---------- общие элементы ----------
    def _pie(self, cx, cy, r, color, frac, outline_only=False):
        """Круг позиции, как у Aeros: заполненный сектор = пройденная часть цикла."""
        c = self.widget_canvas
        if outline_only:
            c.create_oval(cx - r, cy - r, cx + r, cy + r, outline=color, width=max(1, int(r * 0.12)))
            return
        c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=mix(color, BG, 0.65), width=0)
        if frac > 0.001:
            extent = -359.9 * min(1.0, frac)
            c.create_arc(cx - r, cy - r, cx + r, cy + r, start=90, extent=extent, fill=color, width=0)

    def _meter(self, x0, y0, x1, y1, level=0.0):
        """Сегментный индикатор: зелёный, сверху жёлтый и красный."""
        c = self.widget_canvas
        n = max(6, int((y1 - y0) / 6))
        seg = (y1 - y0) / n
        lit = int(round(level * n))
        for i in range(n):
            frac = (i + 1) / n
            k = 2 if frac > 0.9 else (1 if frac > 0.75 else 0)
            col = METER_ON[k] if i < lit else METER_OFF[k]
            sy1 = y1 - i * seg
            c.create_rectangle(x0, sy1 - seg * 0.62, x1, sy1, fill=col, width=0)

    def _master(self, x0, y0, x1, y1):
        lab_h = min((y1 - y0) * 0.08, 18)
        self._text((x0 + x1) / 2, y0 + lab_h / 2, "MASTER", x1 - x0, lab_h, TEXT)
        mw = (x1 - x0) * 0.28
        mid = (x0 + x1) / 2
        self._meter(mid - mw * 1.15, y0 + lab_h * 1.4, mid - mw * 0.15, y1)
        self._meter(mid + mw * 0.15, y0 + lab_h * 1.4, mid + mw * 1.15, y1)

    # ---------- кадр ----------
    def refresh_gui(self):
        m = self.monitors or {}
        c = self.widget_canvas
        c.delete("all")
        w, h = max(self.width, 120), max(self.height, 80)
        now = time.monotonic()

        st = {
            "bpm": float(m.get("o_bpm", 0.0)),
            "bpb": max(1, min(16, int(round(m.get("o_bpb", 4))))),
            "bar": int(m.get("o_bar", 0)),
            "beat": float(m.get("o_beat", 0.0)),
            "src": int(m.get("o_tsrc", 0)),
            "act": int(m.get("o_action", 0)),
            "cnt": int(m.get("o_act_cnt", 0)),
            "track": int(m.get("o_track", 0)) % 4,
            "part": int(m.get("o_part", 0)) % 4,
            "pend": int(m.get("o_pend", -1)),
            "widget": int(m.get("o_widget", 0)),
        }
        if not 0 <= st["act"] < len(ACTIONS):
            st["act"] = 0

        # вспышка на сильной доле: по смене такта из аудиопотока
        if self.last_bar is not None and st["bar"] != self.last_bar and st["src"]:
            self.flash_until = now + FLASH_S
        self.last_bar = st["bar"]
        if self.last_cnt is not None and st["cnt"] != self.last_cnt:
            self.action_until = now + ACTION_HOLD_S
        self.last_cnt = st["cnt"]

        top_h = int(min(max(h * 0.12, 18), 46))
        tab_h = int(min(max(h * 0.11, 16), 40))
        gap = max(2, int(h * 0.012))
        self._tempo_bar(st, 0, 0, w, top_h, now)
        body = (0, top_h + gap, w, h - tab_h - gap)
        if st["widget"] == 1:
            self._mixer(st, *body)
        else:
            self._loop(st, *body)
        self._tabs(st, 0, h - tab_h, w, h, now)

    def _tempo_bar(self, st, x0, y0, x1, y1, now):
        c = self.widget_canvas
        hh, w = y1 - y0, x1 - x0
        if now < self.flash_until:
            c.create_rectangle(x0, y0, x1, y1, fill="#303030", width=0)
        cy = (y0 + y1) / 2
        if st["src"]:
            bpm = st["bpm"]
            s = "%.1f" % bpm if abs(bpm - round(bpm)) > 0.05 else "%d" % round(bpm)
            self._text(x0 + w * 0.015, cy, s, w * 0.13, hh * 0.86, TEXT, "w")
            self._text(x0 + w * 0.15, cy + hh * 0.12, "BPM", w * 0.06, hh * 0.4, DIM, "w")
            beat_i = min(st["bpb"] - 1, max(0, int(st["beat"])))
            self._text(x0 + w * 0.30, cy, "%d.%d" % (st["bar"] + 1, beat_i + 1), w * 0.12, hh * 0.86, TEXT)
            self._text(x0 + w * 0.38, cy + hh * 0.12, "%d/4" % st["bpb"], w * 0.06, hh * 0.4, DIM, "w")
        else:
            beat_i = -1
            self._text(x0 + w * 0.015, cy, "NO TEMPO", w * 0.3, hh * 0.7, RED, "w")
        # точки долей: первая крупнее и ярче, текущая горит
        dx0, dx1 = x0 + w * 0.47, x1 - w * 0.01
        n = st["bpb"]
        step = (dx1 - dx0) / n
        rmax = min(hh * 0.3, step * 0.4)
        for i in range(n):
            r = rmax if i == 0 else rmax * 0.6
            on = i == beat_i
            col = (TEXT if on else "#5a5a5a") if i == 0 else ("#d0d0d0" if on else "#2c2c2c")
            cx = dx0 + step * (i + 0.5)
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=col, width=0)

    def _loop(self, st, x0, y0, x1, y1):
        c = self.widget_canvas
        w = x1 - x0
        row_h = (y1 - y0) / 4.0
        part_w = row_h * 0.5
        master_w = max(34, w * 0.085)
        wx1 = x1 - master_w - 4
        blink = int(time.monotonic() * 3) % 2 == 0
        for i in range(4):
            ry0, ry1 = y0 + i * row_h, y0 + (i + 1) * row_h
            cy = (ry0 + ry1) / 2
            # точка парта (строка i = парт i), как столбик точек у Aeros
            px = x0 + part_w / 2
            pr = row_h * 0.1
            if i == st["part"]:
                s = row_h * 0.19
                c.create_rectangle(px - s, cy - s, px + s, cy + s, outline=DIM, width=1)
                c.create_oval(px - pr, cy - pr, px + pr, cy + pr, fill=GREEN, width=0)
            elif not (i == st["pend"] and not blink):
                col = GREEN if i == st["pend"] else DIM
                c.create_oval(px - pr, cy - pr, px + pr, cy + pr, outline=col, width=max(1, int(pr * 0.35)))
            # круг позиции с номером дорожки, под ним скорость
            active = i == st["track"]
            pie_r = row_h * 0.3
            pcx, pcy = x0 + part_w + row_h * 0.42, ry0 + row_h * 0.4
            color = GREEN if active else EMPTY
            self._pie(pcx, pcy, pie_r, color, 0.0, outline_only=True)
            self._text(pcx, pcy, str(i + 1), pie_r * 1.3, pie_r * 1.3, GREEN if active else DIM)
            self._text(pcx, ry0 + row_h * 0.87, "1.00×", row_h * 0.8, row_h * 0.22, DIM, bold=False)
            # поле волны; пустая дорожка — тонкая средняя линия
            wx0 = x0 + part_w + row_h * 0.9
            c.create_line(wx0, cy, wx1, cy, fill=GRID, width=1)
            if active:
                c.create_rectangle(x0 + part_w + 2, ry0 + 2, wx1, ry1 - 2, outline=GREEN, width=2)
        self._master(wx1 + 6, y0, x1 - 4, y1)

    def _mixer(self, st, x0, y0, x1, y1):
        """Mixer по образцу Aeros; значения уровней появятся на этапе 4."""
        c = self.widget_canvas
        w, hh = x1 - x0, y1 - y0
        master_w = max(34, w * 0.12)
        cw = (w - master_w - 8) / 4.0
        for i in range(4):
            cx0 = x0 + i * cw
            mid = cx0 + cw / 2
            active = i == st["track"]
            col = GREEN if active else EMPTY
            if i:
                c.create_line(cx0, y0, cx0, y1, fill="#1c1c1c", width=1)
            r = min(cw * 0.2, hh * 0.09)
            self._pie(mid, y0 + r + 2, r, col, 0.0, outline_only=True)
            self._text(mid, y0 + r + 2, str(i + 1), r * 1.3, r * 1.3, GREEN if active else DIM)
            # кнопка M
            by0 = y0 + r * 2 + hh * 0.05
            bs = min(cw * 0.3, hh * 0.13)
            c.create_rectangle(mid - bs / 2, by0, mid + bs / 2, by0 + bs, fill=mix(col, BG, 0.3), width=0)
            self._text(mid, by0 + bs / 2, "M", bs * 0.8, bs * 0.8, TEXT)
            # сдвоенный фейдер: две колонки сегментов, белая ручка на 0 дБ
            fy0, fy1 = by0 + bs + hh * 0.06, y1 - 4
            fw = cw * 0.11
            for k in (-1, 1):
                fx = mid + k * fw * 0.9
                n = max(6, int((fy1 - fy0) / 7))
                seg = (fy1 - fy0) / n
                for j in range(n):
                    sy = fy1 - j * seg
                    c.create_rectangle(fx - fw / 2, sy - seg * 0.45, fx + fw / 2, sy, fill="#1e1e1e", width=0)
            ky = fy0 + (fy1 - fy0) * 0.25
            c.create_rectangle(mid - cw * 0.2, ky - 7, mid + cw * 0.2, ky + 7, fill=TEXT, width=0)
        self._master(x1 - master_w, y0, x1 - 4, y1)

    def _tabs(self, st, x0, y0, x1, y1, now):
        """Три вкладки снизу, как подписи педалей у Aeros."""
        c = self.widget_canvas
        gap = 4
        tw = (x1 - x0 - gap * 2) / 3.0
        label, col = ACTIONS[st["act"]]
        if now >= self.action_until:
            col = mix(col, BG, 0.45) if st["act"] else DIM
        items = [("PART %d" % (st["part"] + 1), GREEN),
                 ("TRACK %d" % (st["track"] + 1), GREEN),
                 (label, col)]
        for k, (text, color) in enumerate(items):
            tx0 = x0 + k * (tw + gap)
            c.create_rectangle(tx0, y0, tx0 + tw, y1, fill=TAB, width=0)
            self._text(tx0 + tw / 2, (y0 + y1) / 2, text, tw * 0.9, (y1 - y0) * 0.78, color)


def mix(c1, c2, f):
    """Смесь цветов: f=0 -> c1, f=1 -> c2."""
    a = [int(c1[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(c2[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(x + (y - x) * f) for x, y in zip(a, b))
