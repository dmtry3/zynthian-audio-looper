#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Audio Looper: виджет в стиле экрана Aeros.
#   Loop:  сверху тонкая полоса темпа и шестерёнка; строки дорожек: точка парта, круг позиции
#          с номером, полоса цикла с линией позиции; справа MASTER; снизу три вкладки.
#   Mixer: на каждую дорожку круг, кнопка M и сдвоенный фейдер; справа MASTER.
#   Настройки (шестерёнка): RPO/ROP, Free/Quantize, длины дорожек, fade, reverse, квантование mute.
# Чёрный фон без плашек, цвет несёт состояние. Виджет читает выходные порты (self.monitors),
# настройки пишет через контроллеры Zynthian (это LV2-параметры плагина), ничего не ждёт.
import glob
import math
import mmap
import os
import struct
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
# Пики волн: общая память с плагином, раскладка как PeakShm в core/peaks.h
PK_MAGIC = 0x31504C41
PK_COLS = 512
PK_LAYERS = 8
PK_HDR = 16
PK_TRACK = 16 + 4 * PK_LAYERS + 2 * PK_COLS * (2 + PK_LAYERS)
PK_SUM = 16 + 4 * PK_LAYERS             # смещения внутри дорожки
PK_DUB = PK_SUM + 2 * PK_COLS
PK_LAY = PK_DUB + 2 * PK_COLS
PEAKS_PATH_OVERRIDE = None              # для превью без Zynthian
# Цвета слоёв овердабов (полупрозрачно поверх волны), по номеру слоя
LAYER_COLORS = ["#ff6b6b", "#ffd43b", "#69db7c", "#4dabf7", "#da77f2", "#ff922b", "#38d9a9", "#f783ac"]
FRAME_MS = 33                           # своя перерисовка 30 кадров в секунду
DOUBLE_TAP_S = 0.35                     # двойной тап по фейдеру или панораме — значение по умолчанию
MIN_DB, MAX_DB = -60.0, 6.0             # −60 = −∞


def db_to_frac(db):
    """Шкала фейдера как у Aeros: 0 дБ на 3/4 высоты, вниз — плавно к −∞."""
    if db <= MIN_DB:
        return 0.0
    if db >= 0:
        return 0.75 + 0.25 * min(db, MAX_DB) / MAX_DB
    return 0.75 * (1.0 - math.sqrt(-db / -MIN_DB))


def frac_to_db(f):
    f = max(0.0, min(1.0, f))
    if f >= 0.75:
        return (f - 0.75) / 0.25 * MAX_DB
    if f <= 0.01:
        return MIN_DB
    return MIN_DB * (1.0 - f / 0.75) ** 2


def lin_to_frac(v):
    """Пик (линейный) в положение на шкале фейдера."""
    return db_to_frac(20.0 * math.log10(v)) if v > 1e-4 else 0.0

# Состояния дорожки (порт o_stN, как TrackView::state в core/looper.h)
ST_EMPTY, ST_ARMED, ST_REC, ST_PLAY, ST_DUB = range(5)
ACTION_HOLD_S = 1.5     # сколько подпись последнего действия горит вместо подсказки
FLASH_S = 0.12          # вспышка полосы темпа на сильной доле
FONT_PREFS = ("DejaVu Sans Condensed", "DejaVu Sans", "Liberation Sans Narrow", "Liberation Sans")

# Настройки: (символ LV2-параметра, подпись, список значений, как показать значение)
FADE_STEPS = [10, 20, 50, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 4000]
REV_STEPS = [5, 10, 20, 30, 50, 75, 100, 150, 200, 300, 500, 750, 1000]


def _ms(v):
    return "%d ms" % v if v < 1000 else "%.1f s" % (v / 1000.0)


def _len(v):
    return "FREE" if v == 0 else ("%d BAR" % v if v == 1 else "%d BARS" % v)


SETTINGS = [
    ("rec_order", "REC ORDER", [0, 1], lambda v: ("RPO", "ROP")[v]),
    ("mode", "MODE", [0, 1], lambda v: ("FREE", "QUANTIZE")[v]),
    ("mute_q", "MUTE AT", [0, 1, 2, 3], lambda v: ("NOW", "BEAT", "BAR", "LOOP END")[v]),
    ("part_q", "PART AT", [0, 1, 2, 3], lambda v: ("NOW", "BEAT", "BAR", "LOOP END")[v]),
    ("fade_out", "FADE OUT", FADE_STEPS, _ms),
    ("fade_in", "FADE IN", FADE_STEPS, _ms),
    ("rev_time", "REVERSE", REV_STEPS, _ms),
    ("len1", "LENGTH 1", list(range(17)), _len),
    ("len2", "LENGTH 2", list(range(17)), _len),
    ("len3", "LENGTH 3", list(range(17)), _len),
    ("len4", "LENGTH 4", list(range(17)), _len),
]
SETTING_DEFAULTS = {"rec_order": 0, "mode": 1, "mute_q": 2, "part_q": 3, "fade_out": 200, "fade_in": 200, "rev_time": 50,
                    "len1": 0, "len2": 0, "len3": 0, "len4": 0}


def mix(c1, c2, f):
    """Смесь цветов: f=0 -> c1, f=1 -> c2."""
    a = [int(c1[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(c2[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(int(x + (y - x) * f) for x, y in zip(a, b))


def track_color(state, muted, reversed_, active):
    """Цвет дорожки по приоритету из ТЗ."""
    if state == ST_REC:
        return RED
    if state == ST_DUB:
        return ORANGE
    if state == ST_EMPTY or state == ST_ARMED:
        return GREEN if active else EMPTY
    if muted:
        return GRAY
    if reversed_:
        return PURPLE
    if active:
        return GREEN
    return BLUE


class zynthian_widget_audio_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat', bg=BG)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)
        self.drag = None          # (вид, символ, прямоугольник) пока палец на фейдере или панораме
        self.last_tap = (None, 0.0)
        self.family = None
        self.fonts = {}
        self.last_bar = None
        self.flash_until = 0.0
        self.last_cnt = None
        self.action_until = 0.0
        self.last_widget = None
        self.settings_open = False
        self.hits = []          # (x0, y0, x1, y1, действие, аргумент) для касаний
        self.mm = None
        self.mm_path = None
        self.peaks = [None] * 4  # кэш пиков по дорожкам
        self.geo = {}            # кэш многоугольников волн
        self.mon_t = 0.0         # когда пришли значения портов (между ними позиция досчитывается)
        self.flash_bar = None
        self.after(FRAME_MS, self._tick)

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.refresh_gui()

    # ---------- контроллеры Zynthian (LV2-параметры настроек) ----------
    def _zval(self, sym):
        try:
            return int(round(float(self.processor.controllers_dict[sym].value)))
        except Exception:
            return SETTING_DEFAULTS.get(sym, 0)

    def _zset(self, sym, value):
        try:
            self.processor.controllers_dict[sym].set_value(value if isinstance(value, float) else int(value))
            return True
        except Exception:
            return False

    def _zfloat(self, sym, default=0.0):
        try:
            return float(self.processor.controllers_dict[sym].value)
        except Exception:
            return default

    # ---------- касания ----------
    def _on_press(self, event):
        for (x0, y0, x1, y1, what, arg) in self.hits:
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                if what in ("fader", "pan"):
                    now = time.monotonic()
                    key = (what, arg)
                    if self.last_tap[0] == key and now - self.last_tap[1] < DOUBLE_TAP_S:
                        self._zset(arg, 0.0 if what == "fader" else 0)  # двойной тап: 0 дБ / центр
                        self.last_tap = (None, 0.0)
                        self.drag = None
                    else:
                        self.last_tap = (key, now)
                        self.drag = (what, arg, (x0, y0, x1, y1))
                        self._drag_to(event)
                    self.refresh_gui()
                    return
                if what == "chm":
                    self._zset(arg, 1 - self._zval(arg))
                elif what == "mute":
                    self._zset("tmute", arg)
                elif what == "lim":
                    self._zset("limiter", 1 - self._zval("limiter"))
                elif what == "gear":
                    self.settings_open = not self.settings_open
                elif what == "step":
                    sym, values, d = arg
                    cur = self._zval(sym)
                    # ближайшее значение из списка, затем шаг вперёд или назад
                    i = min(range(len(values)), key=lambda k: abs(values[k] - cur))
                    i = max(0, min(len(values) - 1, i + d))
                    self._zset(sym, values[i])
                self.refresh_gui()
                return

    def _drag_to(self, event):
        if not self.drag:
            return
        what, sym, (x0, y0, x1, y1) = self.drag
        if what == "fader":
            f = (y1 - event.y) / max(1.0, y1 - y0)
            self._zset(sym, round(frac_to_db(f), 1))
        else:
            v = (event.x - (x0 + x1) / 2.0) / max(1.0, (x1 - x0) / 2.0) * 100.0
            v = int(round(max(-100.0, min(100.0, v))))
            self._zset(sym, 0 if abs(v) < 4 else v)  # у центра прилипает к C

    def _on_motion(self, event):
        if self.drag:
            self._drag_to(event)
            self._draw()

    def _on_release(self, event):
        self.drag = None

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
        """Самый крупный шрифт, в котором text влезает в клетку; None, если не влезает совсем.
        Подбор дорогой (замеры текста), поэтому результат кэшируется."""
        key = (text, int(max_w), int(max_h), bold)
        try:
            return self.fit_cache[key]
        except AttributeError:
            self.fit_cache = {}
        except KeyError:
            pass
        if len(self.fit_cache) > 2000:
            self.fit_cache.clear()
        f = self._fit_measure(text, max_w, max_h, bold)
        self.fit_cache[key] = f
        return f

    def _fit_measure(self, text, max_w, max_h, bold):
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
    def _pie(self, cx, cy, r, color, frac, empty):
        """Круг позиции, как у Aeros: заполненный сектор = пройденная часть цикла."""
        c = self.widget_canvas
        if empty:
            c.create_oval(cx - r, cy - r, cx + r, cy + r, outline=color, width=max(1, int(r * 0.12)))
            return
        c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=mix(color, BG, 0.62), width=0)
        if frac > 0.002:
            if frac > 0.998:
                c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=color, width=0)
            else:
                c.create_arc(cx - r, cy - r, cx + r, cy + r, start=90, extent=-360.0 * frac, fill=color, width=0)

    def _meter(self, x0, y0, x1, y1, level=0.0):
        """Сегментный индикатор: зелёный, сверху жёлтый и красный."""
        c = self.widget_canvas
        n = max(6, int((y1 - y0) / 10))
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

    def _lock_icon(self, cx, cy, r):
        """Значок замка (Lock): дужка и корпус."""
        c = self.widget_canvas
        c.create_arc(cx - r * 0.55, cy - r * 1.0, cx + r * 0.55, cy + r * 0.1, start=0, extent=180,
                     style="arc", outline=ORANGE, width=max(2, int(r * 0.28)))
        c.create_rectangle(cx - r * 0.8, cy - r * 0.35, cx + r * 0.8, cy + r * 0.75, fill=ORANGE, width=0)

    def _gear(self, cx, cy, r, color):
        """Значок шестерёнки: зубцы и кольцо."""
        c = self.widget_canvas
        for k in range(8):
            a = k * math.pi / 4
            x, y = cx + math.cos(a) * r * 0.82, cy + math.sin(a) * r * 0.82
            t = r * 0.2
            c.create_rectangle(x - t, y - t, x + t, y + t, fill=color, width=0)
        c.create_oval(cx - r * 0.72, cy - r * 0.72, cx + r * 0.72, cy + r * 0.72, fill=color, width=0)
        c.create_oval(cx - r * 0.3, cy - r * 0.3, cx + r * 0.3, cy + r * 0.3, fill=BG, width=0)

    # ---------- кадр ----------
    def refresh_gui(self):
        """Zynthian обновил значения портов: запоминаем время и рисуем."""
        self.mon_t = time.monotonic()
        self._draw()

    def _tick(self):
        """Своя перерисовка 30 раз в секунду: позиция и доли досчитываются между обновлениями."""
        try:
            if self.winfo_ismapped() and self.monitors:
                self._draw()
        except Exception:
            pass
        try:
            self.after(FRAME_MS, self._tick)
        except Exception:
            pass

    def _draw(self):
        m = self.monitors or {}
        c = self.widget_canvas
        c.delete("all")
        self.hits = []
        w, h = max(self.width, 120), max(self.height, 80)
        now = time.monotonic()
        dt = min(0.5, max(0.0, now - self.mon_t)) if self.mon_t else 0.0

        flags = int(m.get("o_flags", 0))
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
            "parts": int(m.get("o_parts", 0)),
            "rec_next": bool(flags >> 24 & 1),
            "widget": int(m.get("o_widget", 0)),
            "quantize": self._zval("mode") == 1,
            "rop": self._zval("rec_order") == 1,
            "tracks": [],
        }
        for i in range(4):
            st["tracks"].append({
                "state": int(m.get("o_st%d" % (i + 1), 0)),
                "pos": float(m.get("o_pos%d" % (i + 1), 0.0)),
                "len": float(m.get("o_len%d" % (i + 1), 0.0)),
                "muted": bool(flags >> i & 1),
                "reversed": bool(flags >> (4 + i) & 1),
                "stopped": bool(flags >> (16 + i) & 1),
                "locked": bool(flags >> (20 + i) & 1),
            })
        if not 0 <= st["act"] < len(ACTIONS):
            st["act"] = 0
        # между обновлениями портов доли и позиции идут по времени
        if dt > 0:
            beat = st["beat"] + dt * st["bpm"] / 60.0
            st["bar"] += int(beat // st["bpb"])
            st["beat"] = beat % st["bpb"]
            for tr in st["tracks"]:
                if tr["state"] in (ST_PLAY, ST_DUB) and tr["len"] > 0 and not tr["stopped"]:
                    d = dt / tr["len"]
                    tr["pos"] = (tr["pos"] + (-d if tr["reversed"] else d)) % 1.0

        # вспышка на сильной доле: смена такта (досчитанная по позиции из аудиопотока)
        fb = self.flash_bar
        if fb is not None and st["src"] and (st["bar"] > fb or st["bar"] < fb - 1):
            self.flash_until = now + FLASH_S
        if fb is None or st["bar"] > fb or st["bar"] < fb - 1:
            self.flash_bar = st["bar"]
        if self.last_cnt is not None and st["cnt"] != self.last_cnt:
            self.action_until = now + ACTION_HOLD_S
        self.last_cnt = st["cnt"]
        # футсвитч переключил Loop/Mixer: настройки закрываются
        if self.last_widget is not None and st["widget"] != self.last_widget:
            self.settings_open = False
        self.last_widget = st["widget"]

        top_h = int(min(max(h * 0.12, 18), 46))
        tab_h = int(min(max(h * 0.11, 16), 40))
        gap = max(2, int(h * 0.012))
        self._tempo_bar(st, 0, 0, w, top_h, now)
        body = (0, top_h + gap, w, h - tab_h - gap)
        if self.settings_open:
            self._settings(*body)
        elif st["widget"] == 1:
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
        gear_w = hh * 1.3
        dx0, dx1 = x0 + w * 0.47, x1 - gear_w - w * 0.01
        n = st["bpb"]
        step = (dx1 - dx0) / n
        rmax = min(hh * 0.3, step * 0.4)
        for i in range(n):
            r = rmax if i == 0 else rmax * 0.6
            on = i == beat_i
            col = (TEXT if on else "#5a5a5a") if i == 0 else ("#d0d0d0" if on else "#2c2c2c")
            cx = dx0 + step * (i + 0.5)
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=col, width=0)
        # шестерёнка: вкладка настроек
        gx = x1 - gear_w / 2
        self._gear(gx, cy, hh * 0.32, GREEN if self.settings_open else "#9a9a9a")
        self.hits.append((x1 - gear_w, y0, x1, y1, "gear", None))

    def _loop(self, st, x0, y0, x1, y1):
        c = self.widget_canvas
        w = x1 - x0
        row_h = (y1 - y0) / 4.0
        part_w = row_h * 0.5
        master_w = max(34, w * 0.085)
        wx1 = x1 - master_w - 4
        blink = int(time.monotonic() * 3) % 2 == 0
        beats_per_s = st["bpm"] / 60.0 if st["bpm"] > 0 else 2.0
        for i in range(4):
            tr = st["tracks"][i]
            ry0, ry1 = y0 + i * row_h, y0 + (i + 1) * row_h
            cy = (ry0 + ry1) / 2
            # точка парта (строка i = парт i), как столбик точек у Aeros:
            # пустой — контур, записанный — заливка, текущий — зелёный, ожидающий — мигает
            px = x0 + part_w / 2
            pr = row_h * 0.11
            recorded = bool(st["parts"] >> i & 1)
            current, pending = i == st["part"], i == st["pend"]
            if current:
                sq = row_h * 0.2
                c.create_rectangle(px - sq, cy - sq, px + sq, cy + sq, outline=DIM, width=1)
            col = GREEN if (current or pending) else ("#b0b0b0" if recorded else DIM)
            if not (pending and not blink):
                if recorded or current:
                    c.create_oval(px - pr, cy - pr, px + pr, cy + pr, fill=col, width=0)
                else:
                    c.create_oval(px - pr, cy - pr, px + pr, cy + pr, outline=col, width=max(1, int(pr * 0.35)))
            # цвет дорожки по приоритету из ТЗ; ожидание такта мигает красным
            active = i == st["track"]
            state = tr["state"]
            color = track_color(state, tr["muted"], tr["reversed"], active)
            if state == ST_ARMED:
                color = RED if blink else EMPTY
            elif tr["stopped"]:
                color = mix(color, BG, 0.55)  # транспорт стоит: дорожка затихла и ждёт пуска
            has_loop = state in (ST_PLAY, ST_DUB) and tr["len"] > 0
            # круг позиции с номером, под ним скорость
            pie_r = row_h * 0.3
            pcx, pcy = x0 + part_w + row_h * 0.42, ry0 + row_h * 0.4
            frac = tr["pos"] if has_loop else (1.0 if state == ST_REC else 0.0)
            self._pie(pcx, pcy, pie_r, color, frac, empty=not (has_loop or state == ST_REC))
            num_col = TEXT if (has_loop or state == ST_REC) else (GREEN if active else DIM)
            self._text(pcx, pcy, str(i + 1), pie_r * 1.3, pie_r * 1.3, num_col)
            if tr["locked"]:
                self._lock_icon(pcx + pie_r * 1.05, ry0 + row_h * 0.26, row_h * 0.11)
            self._text(pcx, ry0 + row_h * 0.87, "1.00×", row_h * 0.8, row_h * 0.22, DIM, bold=False)
            # волна цикла: сумма цветом дорожки (сыгранное ярче), слои овердабов полупрозрачно поверх
            wx0 = x0 + part_w + row_h * 0.9
            bw = wx1 - wx0
            if state == ST_REC:
                band = row_h * 0.26
                c.create_rectangle(wx0, cy - band, wx1, cy + band, fill=mix(color, BG, 0.7), width=0)
            if has_loop:
                self._waves(i, wx0, wx1, cy, row_h * 0.42, color, tr["pos"], tr["reversed"])
            if has_loop or state == ST_REC:
                if has_loop and st["quantize"]:
                    # тонкие линии долей, толстые — тактов
                    beats = tr["len"] * beats_per_s
                    nb = int(round(beats))
                    if 0 < nb <= 64:
                        for b in range(1, nb):
                            x = wx0 + bw * b / beats
                            bar_line = b % st["bpb"] == 0
                            c.create_line(x, cy - row_h * 0.38, x, cy + row_h * 0.38,
                                          fill="#9a9a9a" if bar_line else "#3c3c3c", width=2 if bar_line else 1)
                if has_loop:
                    c.create_line(wx1 - 1, ry0 + 3, wx1 - 1, ry1 - 3, fill=RED, width=2)  # конец цикла
                    x = wx0 + bw * min(1.0, max(0.0, tr["pos"]))
                    c.create_line(x, ry0 + 3, x, ry1 - 3, fill=TEXT, width=2)              # позиция
                    c.create_oval(x - 3, ry0 + 1, x + 3, ry0 + 7, fill=TEXT, width=0)
                else:
                    self._text(wx0 + bw / 2, cy, "REC %.1f s" % tr["len"], bw * 0.4, row_h * 0.3, TEXT)
            else:
                c.create_line(wx0, cy, wx1, cy, fill=GRID, width=1)
            if active:
                c.create_rectangle(x0 + part_w + 2, ry0 + 2, wx1, ry1 - 2, outline=GREEN, width=2)
        self._master(wx1 + 6, y0, x1 - 4, y1)

    # ---------- волны ----------
    def _peaks_path(self):
        if PEAKS_PATH_OVERRIDE:
            return PEAKS_PATH_OVERRIDE
        try:
            return "/dev/shm/audio_looper_%d" % self.processor.engine.proc.pid
        except Exception:
            files = glob.glob("/dev/shm/audio_looper_*")
            return files[0] if len(files) == 1 else None

    def _open_mm(self):
        path = self._peaks_path()
        if not path or not os.path.exists(path):
            return None
        if path == self.mm_path and self.mm is not None:
            return self.mm
        try:
            if self.mm is not None:
                self.mm.close()
            with open(path, "rb") as f:
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if len(mm) < PK_HDR + 4 * PK_TRACK or struct.unpack_from("<I", mm, 0)[0] != PK_MAGIC:
                mm.close()
                return None
            self.mm, self.mm_path = mm, path
            self.peaks = [None] * 4
            return mm
        except Exception:
            self.mm = None
            return None

    def _read_peaks(self, i):
        """Пики дорожки из общей памяти; перечитываем, только когда плагин их обновил."""
        mm = self._open_mm()
        old = self.peaks[i]
        if mm is None:
            return old
        off = PK_HDR + i * PK_TRACK
        try:
            seq, live, nl, dub = struct.unpack_from("<4I", mm, off)
            if old and old["seq"] == seq and old["live"] == live:
                return old
            if seq & 1:
                return old  # плагин как раз пишет
            fmt = "%db" % PK_COLS
            nos = struct.unpack_from("<%dI" % PK_LAYERS, mm, off + 16)
            d = {"seq": seq, "live": live,
                 "sum": (struct.unpack_from(fmt, mm, off + PK_SUM), struct.unpack_from(fmt, mm, off + PK_SUM + PK_COLS)),
                 "dub": None, "layers": []}
            if dub:
                d["dub"] = (struct.unpack_from(fmt, mm, off + PK_DUB),
                            struct.unpack_from(fmt, mm, off + PK_DUB + PK_COLS))
            for j in range(min(nl, PK_LAYERS)):
                b = off + PK_LAY + j * 2 * PK_COLS
                d["layers"].append((nos[j], struct.unpack_from(fmt, mm, b), struct.unpack_from(fmt, mm, b + PK_COLS)))
            if struct.unpack_from("<I", mm, off)[0] != seq:
                return old
            peak = max(1, max(d["sum"][1]), -min(d["sum"][0]))
            d["gain"] = min(4.0, 120.0 / peak)  # тихий луп тоже видно, но не больше 4x
            self.peaks[i] = d
            return d
        except Exception:
            return old

    def _wave_coords(self, mn, mx, x0, x1, cy, hh, gain):
        """Верхний край слева направо и нижний справа налево (плоский список координат)."""
        n = len(mx)
        step = max(1, int(n / max(16.0, (x1 - x0) / 2.0)))
        k = hh * gain / 127.0
        top, bot = [], []
        for a in range(0, n, step):
            e = min(n, a + step)
            hi, lo = max(mx[a:e]), min(mn[a:e])
            x = x0 + (x1 - x0) * (a + e) / 2.0 / n
            yt, yb = cy - hi * k, cy - lo * k
            if yb - yt < 1.0:
                yt, yb = cy - 0.5, cy + 0.5
            top += [x, max(cy - hh, yt)]
            bot += [x, min(cy + hh, yb)]
        npts = len(top) // 2
        botrev = []
        for j in range(npts - 1, -1, -1):
            botrev += bot[2 * j:2 * j + 2]
        return top, botrev, npts

    def _waves(self, i, x0, x1, cy, hh, color, pos, reversed_):
        d = self._read_peaks(i)
        if not d:
            return
        c = self.widget_canvas
        key = (d["seq"], d["live"], int(x0), int(x1), int(cy), int(hh))
        g = self.geo.get(i)
        if not g or g["key"] != key:
            g = {"key": key,
                 "sum": self._wave_coords(d["sum"][0], d["sum"][1], x0, x1, cy, hh, d["gain"]),
                 "layers": [(no, self._wave_coords(mn, mx, x0, x1, cy, hh, d["gain"]))
                            for (no, mn, mx) in reversed(d["layers"])],
                 "dub": (self._wave_coords(d["dub"][0], d["dub"][1], x0, x1, cy, hh, d["gain"])
                         if d["dub"] else None),
                 "dub_no": (d["layers"][0][0] + 1) if d["layers"] else 1}
            self.geo[i] = g
        top, botrev, n = g["sum"]
        if n < 2:
            return
        # волна ровного цвета дорожки; позицию показывает белая линия
        c.create_polygon(top + botrev, fill=color, outline="")
        # слои овердабов: полупрозрачно поверх (штриховка), старые снизу
        for no, (lt, lb, ln) in g["layers"]:
            if ln >= 2:
                c.create_polygon(lt + lb, fill=LAYER_COLORS[no % len(LAYER_COLORS)], outline="", stipple="gray50")
        if g["dub"] and g["dub"][2] >= 2:
            dt_, db_, _ = g["dub"]
            c.create_polygon(dt_ + db_, fill=LAYER_COLORS[g["dub_no"] % len(LAYER_COLORS)], outline="",
                             stipple="gray50")

    def _mixer(self, st, x0, y0, x1, y1):
        """Mixer по образцу Aeros: 4 дорожки играющего парта, справа Dry и Master."""
        c = self.widget_canvas
        m = self.monitors or {}
        w, hh = x1 - x0, y1 - y0
        side_w = max(70, w * 0.24)
        cw = (w - side_w - 6) / 4.0
        base = st["part"] * 4
        for i in range(4):
            tr = st["tracks"][i]
            n = base + i + 1
            cx0 = x0 + i * cw
            mid = cx0 + cw / 2
            active = i == st["track"]
            col = track_color(tr["state"], tr["muted"], tr["reversed"], active)
            has_loop = tr["state"] in (ST_PLAY, ST_DUB)
            if i:
                c.create_line(cx0, y0, cx0, y1, fill="#1c1c1c", width=1)
            if active:
                c.create_rectangle(cx0 + 2, y0 + 1, cx0 + cw - 2, y1 - 1, outline=GREEN, width=2)
            # круг позиции с номером
            r = min(cw * 0.16, hh * 0.075)
            self._pie(mid, y0 + r + 3, r, col, tr["pos"] if has_loop else 0.0, empty=not has_loop)
            self._text(mid, y0 + r + 3, str(i + 1), r * 1.3, r * 1.3, TEXT if has_loop else col)
            # панорама: полоса, метка центра, бегунок
            pan = self._zval("pan%d" % n)
            py0 = y0 + 2 * r + hh * 0.05
            ph = max(10, hh * 0.07)
            px0, px1 = cx0 + cw * 0.12, cx0 + cw * 0.88
            c.create_rectangle(px0, py0, px1, py0 + ph, fill="#1a1a1a", width=0)
            c.create_line((px0 + px1) / 2, py0, (px0 + px1) / 2, py0 + ph, fill=DIM)
            kx = (px0 + px1) / 2 + (px1 - px0) / 2 * pan / 100.0
            c.create_rectangle(kx - 3, py0 - 1, kx + 3, py0 + ph + 1, fill=TEXT, width=0)
            self.hits.append((px0 - 4, py0 - 6, px1 + 4, py0 + ph + 6, "pan", "pan%d" % n))
            # кнопки: режим канала и M
            by0 = py0 + ph + hh * 0.04
            bh = max(14, hh * 0.09)
            bw = (cw * 0.76 - 4) / 2
            split = self._zval("chm%d" % n) == 1
            bx = cx0 + cw * 0.12
            c.create_rectangle(bx, by0, bx + bw, by0 + bh, fill="#3a2a5a" if split else "#262626", width=0)
            self._text(bx + bw / 2, by0 + bh / 2, "SPLIT" if split else "ST", bw * 0.9, bh * 0.7,
                       PURPLE if split else "#b0b0b0")
            self.hits.append((bx, by0, bx + bw, by0 + bh, "chm", "chm%d" % n))
            mx = bx + bw + 4
            c.create_rectangle(mx, by0, mx + bw, by0 + bh, fill=GRAY if tr["muted"] else mix(col, BG, 0.4), width=0)
            self._text(mx + bw / 2, by0 + bh / 2, "M", bw * 0.8, bh * 0.75, TEXT)
            self.hits.append((mx, by0, mx + bw, by0 + bh, "mute", i + 1))
            # фейдер с индикатором пика и подпись дБ
            lab_h = max(10, hh * 0.07)
            fy0, fy1 = by0 + bh + hh * 0.05, y1 - lab_h - 4
            db = self._zfloat("lev%d" % n)
            self._fader(mid, fy0, fy1, cw, db, float(m.get("o_pk%d" % (i + 1), 0.0)), None, "lev%d" % n)
            self._text(mid, y1 - lab_h / 2 - 2, "-\u221e" if db <= MIN_DB else "%+.1f" % db, cw * 0.8, lab_h, DIM,
                       bold=False)
        # справа: Dry и Master
        sx0 = x1 - side_w
        c.create_line(sx0 - 3, y0, sx0 - 3, y1, fill="#2a2a2a", width=1)
        half = side_w / 2.0
        lab_h = max(10, hh * 0.07)
        top_h = max(14, hh * 0.09)
        for k, (sym, label) in enumerate((("dry", "DRY"), ("master", "MASTER"))):
            mid = sx0 + half * (k + 0.5)
            self._text(mid, y0 + top_h / 2 + 2, label, half * 0.9, top_h * 0.8, TEXT)
            db = self._zfloat(sym)
            if sym == "master":
                lim = self._zval("limiter") == 1
                ly0 = y0 + top_h + 6
                c.create_rectangle(mid - half * 0.4, ly0, mid + half * 0.4, ly0 + top_h,
                                   fill="#2a3a20" if lim else "#262626", width=0)
                self._text(mid, ly0 + top_h / 2, "LIM", half * 0.7, top_h * 0.7, GREEN if lim else DIM)
                self.hits.append((mid - half * 0.4, ly0, mid + half * 0.4, ly0 + top_h, "lim", None))
                pk = (float(m.get("o_mpk_l", 0.0)), float(m.get("o_mpk_r", 0.0)))
            else:
                pk = None
            fy0 = y0 + top_h * 2 + 14
            self._fader(mid, fy0, y1 - lab_h - 4, half, db, pk[0] if pk else None, pk[1] if pk else None, sym)
            self._text(mid, y1 - lab_h / 2 - 2, "-\u221e" if db <= MIN_DB else "%+.1f" % db, half * 0.9, lab_h, DIM,
                       bold=False)

    def _fader(self, mid, fy0, fy1, cw, db, pk_l, pk_r, sym):
        """Сдвоенная шкала сегментов (горит по пику) и белая ручка уровня; тянется пальцем."""
        c = self.widget_canvas
        fw = cw * 0.11
        n = max(6, int((fy1 - fy0) / 8))
        seg = (fy1 - fy0) / n
        for side, pk in ((-1, pk_l), (1, pk_r if pk_r is not None else pk_l)):
            fx = mid + side * fw * 0.9
            lit = int(round(lin_to_frac(pk) * n)) if pk is not None else 0
            for j in range(n):
                frac = (j + 1) / n
                k = 2 if frac > 0.92 else (1 if frac > 0.78 else 0)
                colr = METER_ON[k] if j < lit else "#1e1e1e"
                sy = fy1 - j * seg
                c.create_rectangle(fx - fw / 2, sy - seg * 0.55, fx + fw / 2, sy, fill=colr, width=0)
        # метка 0 дБ
        z = fy1 - (fy1 - fy0) * 0.75
        c.create_line(mid - cw * 0.3, z, mid - cw * 0.22, z, fill=DIM)
        ky = fy1 - (fy1 - fy0) * db_to_frac(db)
        kh = max(5, min(9, (fy1 - fy0) * 0.04))
        c.create_rectangle(mid - cw * 0.2, ky - kh, mid + cw * 0.2, ky + kh, fill=TEXT, width=0)
        self.hits.append((mid - cw * 0.35, fy0 - kh, mid + cw * 0.35, fy1 + kh, "fader", sym))

    def _settings(self, x0, y0, x1, y1):
        """Настройки: две колонки, в строке подпись и значение со стрелками ◀ ▶ по краям."""
        c = self.widget_canvas
        cols, rows = 2, (len(SETTINGS) + 1) // 2
        gap = max(4, int((x1 - x0) * 0.012))
        cw = (x1 - x0 - gap * 3) / cols
        rh = (y1 - y0) / rows
        for k, (sym, label, values, fmt) in enumerate(SETTINGS):
            col, row = k // rows, k % rows
            cx0 = x0 + gap + col * (cw + gap)
            ry0 = y0 + row * rh
            ry1 = ry0 + rh - max(2, rh * 0.08)
            cy = (ry0 + ry1) / 2
            v = self._zval(sym)
            c.create_rectangle(cx0, ry0, cx0 + cw, ry1, fill="#141414", width=0)
            self._text(cx0 + cw * 0.03, cy, label, cw * 0.36, (ry1 - ry0) * 0.5, DIM, "w")
            # значение и стрелки: левая половина уменьшает, правая увеличивает
            vx0, vx1 = cx0 + cw * 0.42, cx0 + cw
            aw = (ry1 - ry0) * 0.9
            changed = v != SETTING_DEFAULTS.get(sym, v)
            try:
                text = fmt(v)
            except Exception:
                text = str(v)
            self._text((vx0 + vx1) / 2, cy, text, (vx1 - vx0) - aw * 2.2, (ry1 - ry0) * 0.62,
                       GREEN if changed else TEXT)
            ah = (ry1 - ry0) * 0.22
            ax = vx0 + aw * 0.5
            c.create_polygon(ax + ah * 0.6, cy - ah, ax + ah * 0.6, cy + ah, ax - ah * 0.6, cy, fill="#9a9a9a")
            ax = vx1 - aw * 0.5
            c.create_polygon(ax - ah * 0.6, cy - ah, ax - ah * 0.6, cy + ah, ax + ah * 0.6, cy, fill="#9a9a9a")
            mid = (vx0 + vx1) / 2
            self.hits.append((vx0, ry0, mid, ry1, "step", (sym, values, -1)))
            self.hits.append((mid, ry0, vx1, ry1, "step", (sym, values, 1)))

    def _tabs(self, st, x0, y0, x1, y1, now):
        """Три вкладки снизу, как у Aeros: парт, дорожка и что сделает следующее нажатие Rec."""
        c = self.widget_canvas
        gap = 4
        tw = (x1 - x0 - gap * 2) / 3.0
        if st["rec_next"] and st["pend"] >= 0:
            hint, hcol = "REC \u2192 PART %d" % (st["pend"] + 1), RED
        elif now < self.action_until and st["act"]:
            hint, hcol = ACTIONS[st["act"]]
        else:
            hint, hcol = self._rec_hint(st)
        if self.settings_open:
            items = [("SETTINGS", TEXT), ("TAP ◀ ▶ TO CHANGE", DIM), ("⚙ CLOSE", GREEN)]
        else:
            part_txt = "PART %d" % (st["part"] + 1)
            if st["pend"] >= 0:
                part_txt += " \u2192 %d" % (st["pend"] + 1)
            items = [(part_txt, GREEN),
                     ("TRACK %d" % (st["track"] + 1), GREEN),
                     (hint, hcol)]
        for k, (text, color) in enumerate(items):
            tx0 = x0 + k * (tw + gap)
            c.create_rectangle(tx0, y0, tx0 + tw, y1, fill=TAB, width=0)
            self._text(tx0 + tw / 2, (y0 + y1) / 2, text, tw * 0.9, (y1 - y0) * 0.78, color)
        if self.settings_open:
            self.hits.append((x0 + 2 * (tw + gap), y0, x1, y1, "gear", None))

    def _rec_hint(self, st):
        """Подсказка, что сделает педаль Rec на активной дорожке (как подписи педалей Aeros)."""
        s = st["tracks"][st["track"]]["state"]
        if s == ST_EMPTY:
            return "RECORD", RED
        if s == ST_ARMED:
            return "WAIT BAR", RED
        if s == ST_REC:
            return ("OVERDUB", ORANGE) if st["rop"] else ("PLAY", BLUE)
        if s == ST_PLAY:
            return "OVERDUB", ORANGE
        return "PLAY", BLUE
