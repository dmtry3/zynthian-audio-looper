#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Audio Looper: виджет (этап 0, раскладка экрана Loop по образцу Aeros).
#   сверху полоса темпа: темп, счёт «такт.доля», точки долей, вспышка сильной доли;
#   слева 4 строки дорожек (пока пустые), активная обведена зелёной рамкой;
#   справа 4 точки партов; последнее нажатие всплывает плашкой поверх дорожек;
#   внизу тонкая строка диагностики (источник темпа, нажатые кнопки).
# Виджет только читает выходные порты плагина (self.monitors), ничего не ждёт.
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

BG = "#000000"
PANEL = "#121212"
PANEL_HI = "#1c1c1c"
TEXT = "#f0f0f0"
TEXT_DIM = "#808080"
TEXT_FAINT = "#4a4a4a"
EMPTY_OUTLINE = "#2c2c2c"
GREEN = "#2ecc5a"
# Цвета дорожек из ТЗ (понадобятся с этапа 1)
COL_REC, COL_DUB, COL_MUTE, COL_REV, COL_PLAY = "#e53935", "#fb8c00", "#757575", "#8e44ad", "#1e88e5"

# Коды порта o_action, как enum Action в lv2/audio_looper.cpp: (подпись, цвет)
ACTIONS = [
    ("", PANEL),
    ("REC / DUB", COL_REC),
    ("UNDO", COL_DUB),
    ("REDO", COL_DUB),
    ("RESET TRACK", COL_MUTE),
    ("RESET ALL", COL_MUTE),
    ("MUTE", COL_MUTE),
    ("REVERSE", COL_REV),
    ("LOCK", "#c9a227"),
    ("NEXT TRACK", GREEN),
    ("PREV TRACK", GREEN),
    ("NEXT PART", COL_PLAY),
    ("PREV PART", COL_PLAY),
    ("PLAY / STOP", "#00acc1"),
    ("LOOP", "#9e9e9e"),
    ("MIXER", "#9e9e9e"),
]
# Биты o_held в порядке контроллеров-действий
BUTTONS = ["Rec", "Undo", "Redo", "Rst", "Mute", "Rev", "Lock", "T+",
           "T-", "P+", "P-", "Play", "Loop", "Mix"]
TEMPO_SRC = ["no tempo", "time:Position", "MIDI clock"]
FLASH_S = 0.12          # вспышка полосы темпа на сильной доле
TOAST_S = 1.2           # сколько висит плашка последнего нажатия


def mix(c1, c2, f):
    """Смесь цветов: f=0 -> c1, f=1 -> c2."""
    a = [int(c1[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(c2[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(x + (y - x) * f) for x, y in zip(a, b))


class zynthian_widget_audio_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat', bg=BG)
        self.widget_canvas.grid(sticky='news')
        self.fonts = {}
        self.last_bar = None
        self.flash_until = 0.0
        self.last_cnt = None
        self.toast_until = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.refresh_gui()

    # ---------- текст, который всегда влезает в свою клетку ----------
    def _font(self, px, bold):
        key = (int(px), bold)
        f = self.fonts.get(key)
        if f is None:
            f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=-max(6, int(px)),
                                  weight="bold" if bold else "normal")
            self.fonts[key] = f
        return f

    def _fit(self, text, max_w, max_h, bold=False):
        """Самый крупный шрифт, в котором text влезает в клетку max_w x max_h.
        None, если не влезает даже самым мелким: тогда надпись не рисуется."""
        px = max(6, int(max_h))
        while True:
            f = self._font(px, bold)
            if f.measure(text) <= max_w and f.metrics("linespace") <= max_h:
                return f
            if px <= 6:
                return None
            px = max(6, int(px * 0.9))

    def _text(self, x, y, text, max_w, max_h, color, anchor="center", bold=False, tag=None):
        if not text or max_w < 4 or max_h < 4:
            return
        f = self._fit(text, max_w, max_h, bold)
        if f is not None:
            self.widget_canvas.create_text(x, y, text=text, fill=color, anchor=anchor, font=f, tags=tag)

    def _rrect(self, x0, y0, x1, y1, r, fill="", outline="", width=1):
        """Прямоугольник со скруглёнными углами."""
        r = max(0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1,
               x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
        self.widget_canvas.create_polygon(pts, smooth=True, fill=fill,
                                          outline=outline, width=width)

    # ---------- отрисовка ----------
    def refresh_gui(self):
        m = self.monitors or {}
        c = self.widget_canvas
        c.delete("all")
        w, h = max(self.width, 80), max(self.height, 60)
        now = time.monotonic()

        st = {
            "bpm": float(m.get("o_bpm", 0.0)),
            "bpb": max(1, min(16, int(round(m.get("o_bpb", 4))))),
            "bar": int(m.get("o_bar", 0)),
            "beat": float(m.get("o_beat", 0.0)),
            "rolling": int(m.get("o_rolling", 0)),
            "src": int(m.get("o_tsrc", 0)),
            "clk_bpm": float(m.get("o_clk_bpm", 0.0)),
            "act": int(m.get("o_action", 0)),
            "cnt": int(m.get("o_act_cnt", 0)),
            "held": int(m.get("o_held", 0)),
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
            self.toast_until = now + TOAST_S
        self.last_cnt = st["cnt"]

        pad = max(3, int(min(w, h) * 0.015))
        top_h = int(h * 0.17)
        diag_h = max(10, int(h * 0.06))
        body = (pad, top_h + pad, w - pad, h - diag_h - pad)

        self._tempo_bar(st, 0, 0, w, top_h, now)
        if st["widget"] == 1:
            self._mixer_stub(st, *body)
        else:
            self._loop_screen(st, *body)
        self._toast(st, *body, now)
        self._diag(st, pad, h - diag_h, w - pad, h)

    def _tempo_bar(self, st, x0, y0, x1, y1, now):
        c = self.widget_canvas
        hh = y1 - y0
        flash = now < self.flash_until
        c.create_rectangle(x0, y0, x1, y1, fill="#3a3a3a" if flash else PANEL_HI, width=0)
        w = x1 - x0
        cy = (y0 + y1) / 2
        if st["src"]:
            bpm = "%.1f" % st["bpm"] if abs(st["bpm"] - round(st["bpm"])) > 0.05 else "%d" % round(st["bpm"])
            self._text(x0 + w * 0.02, cy, bpm, w * 0.16, hh * 0.62, TEXT, "w", True)
            self._text(x0 + w * 0.19, cy, "BPM", w * 0.07, hh * 0.3, TEXT_DIM, "w")
            beat_i = min(st["bpb"] - 1, max(0, int(st["beat"])))
            self._text(x0 + w * 0.38, cy, "%d.%d" % (st["bar"] + 1, beat_i + 1), w * 0.14, hh * 0.62,
                       TEXT, "center", True)
            self._text(x0 + w * 0.50, cy, "%d/4" % st["bpb"], w * 0.06, hh * 0.3, TEXT_DIM, "w")
        else:
            beat_i = -1
            self._text(x0 + w * 0.02, cy, "NO TEMPO", w * 0.3, hh * 0.5, COL_REC, "w", True)
        # точки долей: первая крупнее и ярче, текущая горит
        dx0, dx1 = x0 + w * 0.6, x1 - w * 0.02
        n = st["bpb"]
        step = (dx1 - dx0) / n
        rmax = min(hh * 0.28, step * 0.42)
        for i in range(n):
            r = rmax if i == 0 else rmax * 0.62
            on = i == beat_i
            if i == 0:
                col = "#ffffff" if on else "#5c5c5c"
            else:
                col = "#d8d8d8" if on else "#333333"
            cx = dx0 + step * (i + 0.5)
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=col, width=0)

    def _loop_screen(self, st, x0, y0, x1, y1):
        c = self.widget_canvas
        parts_w = max(24, int((x1 - x0) * 0.08))
        gap = max(3, int((y1 - y0) * 0.025))
        tx1 = x1 - parts_w - gap
        row_h = (y1 - y0 - gap * 3) / 4.0
        rr = row_h * 0.18
        for i in range(4):
            ry0 = y0 + i * (row_h + gap)
            ry1 = ry0 + row_h
            active = i == st["track"]
            self._rrect(x0, ry0, tx1, ry1, rr, fill=PANEL,
                        outline=GREEN if active else EMPTY_OUTLINE, width=3 if active else 1)
            # номер и скорость
            lab_w = row_h * 1.15
            self._text(x0 + lab_w * 0.5, ry0 + row_h * 0.38, str(i + 1), lab_w * 0.8, row_h * 0.56,
                       GREEN if active else TEXT_DIM, "center", True)
            self._text(x0 + lab_w * 0.5, ry0 + row_h * 0.80, "1.00×", lab_w * 0.8, row_h * 0.24,
                       TEXT_FAINT)
            # поле волны: пустая дорожка — тонкая средняя линия
            wx0, wx1 = x0 + lab_w, tx1 - row_h * 0.2
            c.create_line(wx0, (ry0 + ry1) / 2, wx1, (ry0 + ry1) / 2, fill=EMPTY_OUTLINE, width=1)
            self._text((wx0 + wx1) / 2, (ry0 + ry1) / 2, "empty", (wx1 - wx0) * 0.3, row_h * 0.22,
                       TEXT_FAINT)
        # точки партов
        px = x1 - parts_w / 2
        pstep = (y1 - y0) / 4.0
        pr = min(parts_w * 0.36, pstep * 0.3)
        blink = int(time.monotonic() * 3) % 2 == 0
        for p in range(4):
            cy = y0 + pstep * (p + 0.5)
            current = p == st["part"]
            pending = p == st["pend"]
            col = GREEN if current else "#6a6a6a"
            if pending and not blink:
                col = PANEL
            c.create_oval(px - pr, cy - pr, px + pr, cy + pr, outline=col, width=max(2, pr * 0.22))
            self._text(px, cy, str(p + 1), pr * 1.1, pr * 1.0, col, "center", True)

    def _mixer_stub(self, st, x0, y0, x1, y1):
        """Заглушка Mixer: 4 канала играющего парта и мастер, пока без значений."""
        c = self.widget_canvas
        names = ["1", "2", "3", "4", "M"]
        gap = max(4, int((x1 - x0) * 0.015))
        cw = (x1 - x0 - gap * 4) / 5.0
        for i, name in enumerate(names):
            cx0 = x0 + i * (cw + gap)
            active = i == st["track"]
            self._rrect(cx0, y0, cx0 + cw, y1, cw * 0.12, fill=PANEL,
                        outline=GREEN if active else EMPTY_OUTLINE, width=3 if active else 1)
            mid = cx0 + cw / 2
            c.create_line(mid, y0 + (y1 - y0) * 0.12, mid, y1 - (y1 - y0) * 0.18, fill="#2a2a2a",
                          width=max(4, int(cw * 0.08)))
            ky = y0 + (y1 - y0) * 0.30   # положение 0 дБ (примерно)
            c.create_rectangle(mid - cw * 0.28, ky - 6, mid + cw * 0.28, ky + 6, fill="#9e9e9e", width=0)
            self._text(mid, y1 - (y1 - y0) * 0.08, name, cw * 0.6, (y1 - y0) * 0.1,
                       GREEN if active else TEXT_DIM, "center", True)
        self._text(x0 + 6, y0 + 4, "MIXER", (x1 - x0) * 0.2, (y1 - y0) * 0.07, TEXT_FAINT, "nw", True)

    def _toast(self, st, x0, y0, x1, y1, now):
        """Плашка последнего нажатия поверх дорожек."""
        if now >= self.toast_until or not st["act"]:
            return
        label, col = ACTIONS[st["act"]]
        w, h = (x1 - x0) * 0.56, (y1 - y0) * 0.3
        cx, cy = (x0 + x1) / 2 - (x1 - x0) * 0.04, (y0 + y1) / 2
        self._rrect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, h * 0.25, fill=col)
        self._text(cx, cy, label, w * 0.86, h * 0.5, "#ffffff", "center", True)

    def _diag(self, st, x0, y0, x1, y1):
        """Строка диагностики этапа 0: источник темпа, транспорт, нажатые кнопки."""
        c = self.widget_canvas
        hh = y1 - y0
        cy = (y0 + y1) / 2
        w = x1 - x0
        src = TEMPO_SRC[st["src"]] if 0 <= st["src"] < 3 else "?"
        info = "%s · %s" % (src, "run" if st["rolling"] else "stop")
        if st["clk_bpm"] > 0:
            info += " · clk %.1f" % st["clk_bpm"]
        self._text(x0, cy, info, w * 0.4, hh * 0.8, TEXT_FAINT, "w")
        # нажатые сейчас кнопки: видно, приходит ли отпускание
        bx0 = x0 + w * 0.45
        step = (x1 - bx0) / len(BUTTONS)
        for i, name in enumerate(BUTTONS):
            down = (st["held"] >> i) & 1
            cx = bx0 + step * (i + 0.5)
            if down:
                self._rrect(cx - step * 0.46, y0 + 1, cx + step * 0.46, y1 - 1, hh * 0.3, fill="#d0d0d0")
            self._text(cx, cy, name, step * 0.86, hh * 0.75, "#000000" if down else TEXT_FAINT)
