#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Audio Looper: виджет в стиле экрана Aeros.
#   Loop:  сверху тонкая полоса темпа и шестерёнка; строки дорожек: точка парта, круг позиции
#          с номером, полоса цикла с линией позиции; справа MASTER; снизу три вкладки.
#   Mixer: на каждую дорожку круг, кнопка M и сдвоенный фейдер; справа MASTER.
#   Настройки (шестерёнка): разделы с сегментами и ползунками, прокрутка пальцем.
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
PK_MAGIC = 0x33504C41                   # "ALP3": 6 дорожек, с волной записи
PK_TRACKS = 6
PK_COLS = 512
PK_LAYERS = 8
PK_HDR = 16
PK_TRACK = 24 + 4 * PK_LAYERS + 2 * PK_COLS * (3 + PK_LAYERS)
PK_SUM = 24 + 4 * PK_LAYERS             # смещения внутри дорожки
PK_DUB = PK_SUM + 2 * PK_COLS
PK_REC = PK_DUB + 2 * PK_COLS
PK_LAY = PK_REC + 2 * PK_COLS
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


SPEED_MIN_ST, SPEED_MAX_ST = -24.0, 12.0     # 0,25× .. 2× в полутонах
SNAP_NAMES = ("FREE", "SEMI", "OCT")
GLIDE_STEPS = [0, 50, 100, 150, 250, 400, 600, 1000, 1500, 2000]


def speed_snap(sp, snap):
    """Как snap_speed в ядре: полутоны или октавы."""
    if snap == 0 or sp <= 0:
        return sp
    st = 12.0 * math.log2(sp)
    q = 12.0 if snap == 2 else 1.0
    return 2.0 ** (round(st / q) * q / 12.0)


def lin_to_frac(v):
    """Пик (линейный) в положение на шкале фейдера."""
    return db_to_frac(20.0 * math.log10(v)) if v > 1e-4 else 0.0

# Состояния дорожки (порт o_stN, как TrackView::state в core/looper.h)
ST_EMPTY, ST_ARMED, ST_REC, ST_PLAY, ST_DUB = range(5)
ACTION_HOLD_S = 1.5     # сколько подпись последнего действия горит вместо подсказки
FLASH_S = 0.2           # вспышка полосы темпа на сильной доле: ярко и гаснет
FONT_PREFS = ("DejaVu Sans Condensed", "DejaVu Sans", "Liberation Sans Narrow", "Liberation Sans")

# Настройки: (символ LV2-параметра, подпись, список значений, как показать значение)
FADE_STEPS = [10, 20, 50, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 4000]
REV_STEPS = [5, 10, 20, 30, 50, 75, 100, 150, 200, 300, 500, 750, 1000]


def _ms(v):
    return "%d ms" % v if v < 1000 else "%.1f s" % (v / 1000.0)


def _len(v):
    return "FREE" if v == 0 else ("%d BAR" % v if v == 1 else "%d BARS" % v)


QUANT_NAMES = ("NOW", "BEAT", "BAR", "LOOP END")
LAT_STEPS = [0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 80, 100]
HOLD_STEPS = [200, 300, 400, 500, 600, 750, 1000, 1250, 1500, 2000]
RHOLD_STEPS = [500, 750, 1000, 1250, 1500, 2000, 2500, 3000]
# Экран настроек: разделы, в разделе строки.
#   ("seg", символ, подпись, названия вариантов[, значения]) — сегментный переключатель;
#      значение = номер варианта или значение из списка
#   ("sl", символ, подпись, список значений, как показать) — ползунок по шагам списка
GRID_NAMES = ("2\u00d72", "4\u00d74", "6\u00d76")
GRID_CONFIRM_S = 3.0    # смена сетки или пула при записанных лупах: второе касание в течение 3 с
CONFIRM_SYMS = ("grid", "pool_mb")   # эти настройки стирают лупы
POOL_MB = [256, 512, 1024, 2048, 4096]
SETTINGS = [
    ("GRID", [
        ("seg", "grid", "PARTS \u00d7 TRACKS", GRID_NAMES),
    ]),
    ("RECORDING", [
        ("seg", "rec_order", "REC ORDER", ("RPO", "ROP")),
        ("seg", "mode", "MODE", ("FREE", "QUANTIZE")),
        ("sl", "latency", "LATENCY", LAT_STEPS, lambda v: "OFF" if v == 0 else "%d ms" % v),
    ]),
    ("QUANTIZE", [
        ("seg", "mute_q", "MUTE AT", QUANT_NAMES),
        ("seg", "part_q", "PART AT", QUANT_NAMES),
        ("sl", "bpb", "BEATS / BAR", list(range(1, 17)), lambda v: "%d" % v),
    ]),
    ("FADES", [
        ("sl", "fade_out", "FADE OUT", FADE_STEPS, _ms),
        ("sl", "fade_in", "FADE IN", FADE_STEPS, _ms),
        ("sl", "rev_time", "REVERSE", REV_STEPS, _ms),
    ]),
    ("TEMPO & SPEED", [
        ("sl", "tempo_glide", "TEMPO GLIDE", [100, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 4000], _ms),
        ("seg", "spd_snap", "SPEED SNAP", SNAP_NAMES),
        ("sl", "spd_glide", "SPEED GLIDE", GLIDE_STEPS, lambda v: "OFF" if v == 0 else _ms(v)),
    ]),
    ("TRACK LENGTH", [
        ("sl", "len%d" % k, "TRACK %d" % k, list(range(17)), _len) for k in range(1, 7)
    ]),
    ("FOOTSWITCH", [
        ("sl", "hold_ms", "HOLD", HOLD_STEPS, _ms),
        ("sl", "reset_ms", "RESET HOLD", RHOLD_STEPS, _ms),
    ]),
    ("OUTPUT", [
        ("seg", "limiter", "LIMITER", ("OFF", "ON")),
    ]),
    ("MEMORY", [
        ("seg", "pool_mb", "POOL", ("256M", "512M", "1G", "2G", "4G"), POOL_MB),
    ]),
]
SCROLL_START_PX = 8     # палец ушёл по вертикали дальше — это прокрутка, а не касание
SETTING_DEFAULTS = {"rec_order": 0, "mode": 1, "mute_q": 2, "part_q": 3, "tempo_glide": 500, "spd_snap": 0,
                    "spd_glide": 150, "fade_out": 200, "fade_in": 200, "rev_time": 50,
                    "len1": 0, "len2": 0, "len3": 0, "len4": 0, "len5": 0, "len6": 0, "limiter": 1, "grid": 1,
                    "latency": 0, "bpb": 4, "hold_ms": 500, "reset_ms": 1000,
                    "pool_mb": 2048}


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


class _Frame:
    """Кадр как список фигур: код рисования вызывает create_* как у холста, а на холст уходят только
    отличия от прошлого кадра (сдвинуть линию, сменить цвет). Перерисовывать всё 30 раз в секунду
    дорого: Tk-вызовы — почти всё время кадра. Фигуры сгруппированы по разделам (полоса темпа, строка
    дорожки…): если в разделе фигура появилась или пропала, сдвиг номеров не трогает другие разделы."""

    def __init__(self, canvas):
        self.canvas = canvas
        self.prev = {}      # раздел -> [(id, тип, координаты, опции)]
        self.order = []     # разделы прошлого кадра
        self.cur = None

    def begin(self):
        self.ops = {}
        self.secs = []
        self.section("base")

    def section(self, name):
        if name not in self.ops:
            self.ops[name] = []
            self.secs.append(name)
        self.cur = self.ops[name]

    def _rec(self, typ, args, kw):
        coords = []
        for a in args:
            if isinstance(a, (list, tuple)):
                coords.extend(round(float(v), 1) for v in a)
            else:
                coords.append(round(float(a), 1))
        self.cur.append((typ, tuple(coords), kw))

    def create_rectangle(self, *a, **kw): self._rec("rectangle", a, kw)
    def create_oval(self, *a, **kw): self._rec("oval", a, kw)
    def create_line(self, *a, **kw): self._rec("line", a, kw)
    def create_polygon(self, *a, **kw): self._rec("polygon", a, kw)
    def create_arc(self, *a, **kw): self._rec("arc", a, kw)
    def create_text(self, *a, **kw): self._rec("text", a, kw)
    def create_image(self, *a, **kw): self._rec("image", a, kw)

    def commit(self):
        c = self.canvas
        new = {}
        last = None    # фигура, поверх которой ставить новую (порядок слоёв как в коде рисования)
        for name in self.secs:
            old = self.prev.get(name, [])
            out = []
            for i, (typ, coords, kw) in enumerate(self.ops[name]):
                o = old[i] if i < len(old) else None
                if o is not None and o[1] == typ and not (set(o[3]) - set(kw)):
                    iid = o[0]
                    if o[2] != coords:
                        c.coords(iid, *coords)
                    if o[3] != kw:
                        c.itemconfigure(iid, **{k: v for k, v in kw.items() if o[3].get(k) != v})
                else:
                    if o is not None:
                        c.delete(o[0])
                    iid = getattr(c, "create_" + typ)(*coords, **kw)
                    if last is not None:
                        c.tag_raise(iid, last)
                    else:
                        c.tag_lower(iid)
                out.append((iid, typ, coords, kw))
                last = iid
            for o in old[len(out):]:
                c.delete(o[0])
            new[name] = out
        for name in self.order:
            if name not in new:
                for o in self.prev[name]:
                    c.delete(o[0])
        self.prev, self.order = new, list(self.secs)


class zynthian_widget_audio_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat', bg=BG)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)
        self.cv = _Frame(self.widget_canvas)  # всё рисование — через него
        self.drag = None          # (вид, символ, прямоугольник) пока палец на фейдере или панораме
        self.speed_slot = None    # открыт экран Speed этой дорожки (0..3)
        self.last_tap = (None, 0.0)
        self.family = None
        self.fonts = {}
        self.last_bar = None
        self.flash_until = 0.0
        self.last_cnt = None
        self.action_until = 0.0
        self.last_widget = None
        self.settings_open = False
        self.sdrag = None         # касание на экране настроек: прокрутка, ползунок или выбор
        self.s_scroll = 0.0       # прокрутка настроек, px
        self.s_max = 0.0
        self.hits = []          # (x0, y0, x1, y1, действие, аргумент) для касаний
        self.mm = None
        self.mm_path = None
        self.peaks = [None] * PK_TRACKS  # кэш пиков по дорожкам
        self.grid_confirm = None  # (символ, вариант, до какого времени ждём второго касания)
        self.geo = {}            # картинки волн по строкам
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
                if what in ("sseg", "ssl", "sbody"):
                    # что это было, станет ясно по движению пальца: прокрутка или ползунок
                    self.sdrag = {"what": what, "arg": arg, "x": event.x, "y": event.y,
                                  "scroll": self.s_scroll, "mode": None}
                    return
                if what == "speed":  # метка скорости у дорожки: экран Speed
                    self.speed_slot = arg
                    self.settings_open = False
                    self.refresh_gui()
                    return
                if what == "sclose":
                    self.speed_slot = None
                    self.refresh_gui()
                    return
                if what == "sreset":
                    self._zset("sreset", arg)
                    self.refresh_gui()
                    return
                if what == "snap":
                    self._zset("spd_snap", (self._zval("spd_snap") + 1) % 3)
                    self.refresh_gui()
                    return
                if what == "sglide":
                    cur = self._zval("spd_glide")
                    i = min(range(len(GLIDE_STEPS)), key=lambda k: abs(GLIDE_STEPS[k] - cur))
                    self._zset("spd_glide", GLIDE_STEPS[(i + 1) % len(GLIDE_STEPS)])
                    self.refresh_gui()
                    return
                if what == "sval":  # двойной тап по значению скорости — сброс
                    now = time.monotonic()
                    if self.last_tap[0] == ("sval", arg) and now - self.last_tap[1] < DOUBLE_TAP_S:
                        self._zset("sreset", arg)
                        self.last_tap = (None, 0.0)
                    else:
                        self.last_tap = (("sval", arg), now)
                    self.refresh_gui()
                    return
                if what == "sslider":
                    self.drag = (what, arg, (x0, y0, x1, y1))
                    self._drag_to(event)
                    self.refresh_gui()
                    return
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
                    self._zset(arg, (self._zval(arg) + 1) % 3)  # ST -> L -> R
                elif what == "mute":
                    self._zset("tmute", arg)
                elif what == "lim":
                    self._zset("limiter", 1 - self._zval("limiter"))
                elif what == "gear":
                    self.settings_open = not self.settings_open
                    self.s_scroll = 0.0
                self.refresh_gui()
                return

    def _drag_to(self, event):
        if not self.drag:
            return
        what, sym, (x0, y0, x1, y1) = self.drag
        if what == "sslider":
            f = (event.x - x0) / max(1.0, x1 - x0)
            st = SPEED_MIN_ST + max(0.0, min(1.0, f)) * (SPEED_MAX_ST - SPEED_MIN_ST)
            sp = speed_snap(2.0 ** (st / 12.0), self._zval("spd_snap"))
            if abs(12.0 * math.log2(sp)) < 0.15:
                sp = 1.0  # у 1× прилипает
            self._zset(sym, round(max(0.25, min(2.0, sp)), 4))
            return
        if what == "fader":
            f = (y1 - event.y) / max(1.0, y1 - y0)
            self._zset(sym, round(frac_to_db(f), 1))
        else:
            v = (event.x - (x0 + x1) / 2.0) / max(1.0, (x1 - x0) / 2.0) * 100.0
            v = int(round(max(-100.0, min(100.0, v))))
            self._zset(sym, 0 if abs(v) < 4 else v)  # у центра прилипает к C

    def _slide(self, arg, x):
        """Ползунок настроек: положение пальца -> ближайший шаг списка."""
        sym, values, tx0, tx1 = arg
        f = max(0.0, min(1.0, (x - tx0) / max(1.0, tx1 - tx0)))
        v = values[int(round(f * (len(values) - 1)))]
        if v != self._zval(sym):
            self._zset(sym, v)

    def _tap_confirm(self, sym, k, value):
        """Смена сетки или пула стирает все лупы: если они есть, нужно второе касание в течение 3 с."""
        now = time.monotonic()
        if value == self._zval(sym):
            self.grid_confirm = None
            return
        loops = int((self.monitors or {}).get("o_parts", 0)) != 0
        gc = self.grid_confirm
        if not loops or (gc and gc[0] == sym and gc[1] == k and now < gc[2]):
            self._zset(sym, value)
            self.grid_confirm = None
        else:
            self.grid_confirm = (sym, k, now + GRID_CONFIRM_S)

    def _on_motion(self, event):
        d = self.sdrag
        if d:
            dx, dy = event.x - d["x"], event.y - d["y"]
            if d["mode"] is None:
                if abs(dy) >= SCROLL_START_PX and abs(dy) > abs(dx):
                    d["mode"] = "scroll"
                elif d["what"] == "ssl" and abs(dx) >= 4:
                    d["mode"] = "slide"
            if d["mode"] == "scroll":
                self.s_scroll = max(0.0, min(self.s_max, d["scroll"] - dy))
            elif d["mode"] == "slide":
                self._slide(d["arg"], event.x)
            if d["mode"]:
                self._draw()
            return
        if self.drag:
            self._drag_to(event)
            self._draw()

    def _on_release(self, event):
        self.drag = None
        d, self.sdrag = self.sdrag, None
        if d and d["mode"] is None and event is not None:  # простое касание
            if d["what"] == "sseg" and d["arg"][0] in CONFIRM_SYMS:
                self._tap_confirm(*d["arg"])
            elif d["what"] == "sseg":
                self._zset(d["arg"][0], d["arg"][2])
            elif d["what"] == "ssl":
                self._slide(d["arg"], event.x)
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
            self.cv.create_text(x, y, text=text, fill=color, anchor=anchor, font=f)

    # ---------- общие элементы ----------
    def _pie(self, cx, cy, r, color, frac, empty):
        """Круг позиции, как у Aeros: заполненный сектор = пройденная часть цикла.
        У пустой дорожки круга нет, остаётся только номер."""
        c = self.cv
        if empty:
            return
        c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=mix(color, BG, 0.62), width=0)
        if frac > 0.002:
            if frac > 0.998:
                c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=color, width=0)
            else:
                c.create_arc(cx - r, cy - r, cx + r, cy + r, start=90, extent=-360.0 * frac, fill=color, width=0)

    def _meter(self, x0, y0, x1, y1, level=0.0):
        """Сегментный индикатор: зелёный, сверху жёлтый и красный."""
        c = self.cv
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
        self.cv.section("master")
        lab_h = min((y1 - y0) * 0.08, 18)
        self._text((x0 + x1) / 2, y0 + lab_h / 2, "MASTER", x1 - x0, lab_h, TEXT)
        mw = (x1 - x0) * 0.28
        mid = (x0 + x1) / 2
        self._meter(mid - mw * 1.15, y0 + lab_h * 1.4, mid - mw * 0.15, y1)
        self._meter(mid + mw * 0.15, y0 + lab_h * 1.4, mid + mw * 1.15, y1)

    def _lock_icon(self, cx, cy, r):
        """Значок замка (Lock): дужка и корпус."""
        c = self.cv
        c.create_arc(cx - r * 0.55, cy - r * 1.0, cx + r * 0.55, cy + r * 0.1, start=0, extent=180,
                     style="arc", outline=ORANGE, width=max(2, int(r * 0.28)))
        c.create_rectangle(cx - r * 0.8, cy - r * 0.35, cx + r * 0.8, cy + r * 0.75, fill=ORANGE, width=0)

    def _gear(self, cx, cy, r, color):
        """Значок шестерёнки: зубцы и кольцо."""
        c = self.cv
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
        c = self.cv
        c.begin()
        self.hits = []
        w, h = max(self.width, 120), max(self.height, 80)
        now = time.monotonic()
        dt = min(0.5, max(0.0, now - self.mon_t)) if self.mon_t else 0.0

        flags = int(m.get("o_flags", 0))
        n = int(round(m.get("o_grid", 4)))
        if n not in (2, 4, 6):
            n = 4

        def mask(key, shift):  # флаги дорожек: маски плагина, у старого плагина — упакованный o_flags
            v = m.get(key)
            return int(v) if v is not None else (flags >> shift) & 15
        mutes, revs, stops, locks = mask("o_mutes", 0), mask("o_revs", 4), mask("o_stops", 16), mask("o_locks", 20)
        st = {
            "bpm": float(m.get("o_bpm", 0.0)),
            "bpb": max(1, min(16, int(round(m.get("o_bpb", 4))))),
            "bar": int(m.get("o_bar", 0)),
            "beat": float(m.get("o_beat", 0.0)),
            "src": int(m.get("o_tsrc", 0)),
            "act": int(m.get("o_action", 0)),
            "cnt": int(m.get("o_act_cnt", 0)),
            "track": int(m.get("o_track", 0)) % n,
            "part": int(m.get("o_part", 0)) % n,
            "n": n,
            "pend": int(m.get("o_pend", -1)),
            "parts": int(m.get("o_parts", 0)),
            "rec_next": bool(flags >> 24 & 1),
            "widget": int(m.get("o_widget", 0)),
            "quantize": self._zval("mode") == 1,
            "rop": self._zval("rec_order") == 1,
            "tracks": [],
        }
        for i in range(n):
            st["tracks"].append({
                "state": int(m.get("o_st%d" % (i + 1), 0)),
                "pos": float(m.get("o_pos%d" % (i + 1), 0.0)),
                "len": float(m.get("o_len%d" % (i + 1), 0.0)),
                "muted": bool(mutes >> i & 1),
                "reversed": bool(revs >> i & 1),
                "stopped": bool(stops >> i & 1),
                "locked": bool(locks >> i & 1),
                "speed": float(m.get("o_spd%d" % (i + 1), 1.0)),
                "espeed": float(m.get("o_espd%d" % (i + 1), 1.0)),
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
                    d = dt / tr["len"] * max(0.0, tr["espeed"])
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
            self.speed_slot = None
        self.last_widget = st["widget"]

        top_h = int(min(max(h * 0.12, 18), 46))
        tab_h = int(min(max(h * 0.11, 16), 40))
        gap = max(2, int(h * 0.012))
        body = (0, top_h + gap, w, h - tab_h - gap)
        self.cur_n = st["n"]
        if self.speed_slot is not None and self.speed_slot >= st["n"]:
            self.speed_slot = None
        if self.speed_slot is None and self.settings_open:
            # настройки прокручиваются: строки за краем закрываются полосой темпа и вкладками
            self._settings(*body)
            c.create_rectangle(0, 0, w, body[1], fill=BG, width=0)
            c.create_rectangle(0, body[3], w, h, fill=BG, width=0)
            self._tempo_bar(st, 0, 0, w, top_h, now)
        else:
            self._tempo_bar(st, 0, 0, w, top_h, now)
        if self.speed_slot is not None:
            self._speed_view(st, *body)
        elif self.settings_open:
            pass
        elif st["widget"] == 1:
            self._mixer(st, *body)
        else:
            self._loop(st, *body)
        self._tabs(st, 0, h - tab_h, w, h, now)
        c.commit()

    def _tempo_bar(self, st, x0, y0, x1, y1, now):
        c = self.cv
        c.section("tempo")
        hh, w = y1 - y0, x1 - x0
        if now < self.flash_until:  # вспышка: сразу светло-серый, гаснет к чёрному
            v = int(24 + 120 * (self.flash_until - now) / FLASH_S)
            c.create_rectangle(x0, y0, x1, y1, fill="#%02x%02x%02x" % (v, v, v), width=0)
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
        # сильная доля заметно ярче и крупнее остальных: белая с ореолом, слабые — серые
        for i in range(n):
            r = rmax if i == 0 else rmax * 0.55
            on = i == beat_i
            col = (TEXT if on else "#4a4a4a") if i == 0 else ("#8a8a8a" if on else "#262626")
            cx = dx0 + step * (i + 0.5)
            if i == 0 and on:
                g = r * 1.35
                c.create_oval(cx - g, cy - g, cx + g, cy + g, outline="#b0b0b0", width=2)
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=col, width=0)
        # шестерёнка: вкладка настроек
        gx = x1 - gear_w / 2
        self._gear(gx, cy, hh * 0.32, GREEN if self.settings_open else "#9a9a9a")
        self.hits.append((x1 - gear_w, y0, x1, y1, "gear", None))

    def _loop(self, st, x0, y0, x1, y1):
        c = self.cv
        w = x1 - x0
        n = st["n"]
        row_h = (y1 - y0) / float(n)
        u = min(row_h, (y1 - y0) / 4.0)   # размер значков: в 2×2 строки высокие, значки как в 4×4
        part_w = u * 0.5
        master_w = max(34, w * 0.085)
        wx1 = x1 - master_w - 4
        blink = int(time.monotonic() * 3) % 2 == 0
        beats_per_s = st["bpm"] / 60.0 if st["bpm"] > 0 else 2.0
        for i in range(n):
            c.section("row%d" % i)
            tr = st["tracks"][i]
            ry0, ry1 = y0 + i * row_h, y0 + (i + 1) * row_h
            cy = (ry0 + ry1) / 2
            # точка парта (строка i = парт i), как столбик точек у Aeros:
            # пустой — контур, записанный — заливка, текущий — зелёный, ожидающий — мигает
            px = x0 + part_w / 2
            pr = u * 0.11
            recorded = bool(st["parts"] >> i & 1)
            current, pending = i == st["part"], i == st["pend"]
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
            pie_r = u * 0.3
            pcx, pcy = x0 + part_w + u * 0.42, cy - u * 0.1
            frac = tr["pos"] if has_loop else (1.0 if state == ST_REC else 0.0)
            self._pie(pcx, pcy, pie_r, color, frac, empty=not (has_loop or state == ST_REC))
            num_col = TEXT if (has_loop or state == ST_REC) else (GREEN if active else DIM)
            self._text(pcx, pcy, str(i + 1), pie_r * 1.3, pie_r * 1.3, num_col)
            if tr["locked"]:
                self._lock_icon(pcx + pie_r * 1.05, cy - u * 0.24, u * 0.11)
            sp = tr["speed"]
            self._text(pcx, cy + u * 0.37, "%.2f×" % sp, u * 0.8, u * 0.22,
                       ORANGE if abs(sp - 1.0) > 1e-3 else DIM, bold=abs(sp - 1.0) > 1e-3)
            self.hits.append((x0 + part_w, cy + u * 0.22, x0 + part_w + u * 0.85, cy + u * 0.5, "speed", i))
            # волна цикла: сумма цветом дорожки (сыгранное ярче), слои овердабов полупрозрачно поверх
            wx0 = x0 + part_w + u * 0.9
            bw = wx1 - wx0
            if state == ST_REC:
                self._rec_wave(st, i, wx0, wx1, cy, row_h, color, u)
            if has_loop:
                self._waves(i, wx0, wx1, cy, row_h * 0.42, color, tr["pos"], tr["reversed"])
            if has_loop:
                if st["quantize"]:
                    # тонкие линии долей, толстые — тактов
                    beats = tr["len"] * beats_per_s
                    nb = int(round(beats))
                    if 0 < nb <= 64:
                        for b in range(1, nb):
                            x = wx0 + bw * b / beats
                            bar_line = b % st["bpb"] == 0
                            c.create_line(x, cy - row_h * 0.38, x, cy + row_h * 0.38,
                                          fill="#9a9a9a" if bar_line else "#3c3c3c", width=2 if bar_line else 1)
                c.create_line(wx1 - 1, ry0 + 3, wx1 - 1, ry1 - 3, fill=RED, width=2)  # конец цикла
                x = wx0 + bw * min(1.0, max(0.0, tr["pos"]))
                c.create_line(x, ry0 + 3, x, ry1 - 3, fill=TEXT, width=2)              # позиция
                c.create_oval(x - 3, ry0 + 1, x + 3, ry0 + 7, fill=TEXT, width=0)
            elif state != ST_REC:
                c.create_line(wx0, cy, wx1, cy, fill=GRID, width=1)
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
            if len(mm) < PK_HDR + PK_TRACKS * PK_TRACK or struct.unpack_from("<I", mm, 0)[0] != PK_MAGIC:
                mm.close()
                return None
            self.mm, self.mm_path = mm, path
            self.peaks = [None] * PK_TRACKS
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
            nos = struct.unpack_from("<%dI" % PK_LAYERS, mm, off + 24)
            d = {"seq": seq, "live": live,
                 "sum": (struct.unpack_from(fmt, mm, off + PK_SUM), struct.unpack_from(fmt, mm, off + PK_SUM + PK_COLS)),
                 "dub": None, "layers": [], "rec": None}
            rec_cols, rec_col_s = struct.unpack_from("<If", mm, off + 16)
            if 0 < rec_cols <= PK_COLS:  # идёт запись основы
                d["rec"] = (rec_cols, rec_col_s, struct.unpack_from("%db" % rec_cols, mm, off + PK_REC),
                            struct.unpack_from("%db" % rec_cols, mm, off + PK_REC + PK_COLS))
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

    def _rec_wave(self, st, i, x0, x1, cy, row_h, color, u):
        """Запись основы: волна растёт слева направо, черты тактов, счёт тактов.
        Фиксированная длина — вся полоса и есть цикл; свободная — полоса заполняется,
        потом колонки сливаются вдвое (это делает ядро) и волна сжимается к левой половине."""
        c = self.cv
        tr = st["tracks"][i]
        bw = x1 - x0
        hh = row_h * 0.42
        c.create_rectangle(x0, cy - hh, x1, cy + hh, fill=mix(color, BG, 0.88), width=0)
        d = self._read_peaks(i)
        rec = d.get("rec") if d else None
        n, col_s = (rec[0], rec[1]) if rec else (0, 0.0)
        span_s = col_s * PK_COLS if col_s > 0 else 0.0     # секунд во всю ширину полосы
        bars_txt = None
        if st["quantize"] and st["bpm"] > 0:
            bar_s = st["bpb"] * 60.0 / st["bpm"]
            if span_s > 0 and bar_s > 0 and span_s / bar_s <= 64:
                k = 1
                while k * bar_s < span_s - 1e-3:     # черты тактов
                    x = x0 + bw * k * bar_s / span_s
                    c.create_line(x, cy - hh, x, cy + hh, fill="#5a2020", width=2)
                    k += 1
            done = int(tr["len"] / bar_s + 1e-6) if bar_s > 0 else 0
            target = self._zval("len%d" % (i + 1))
            if target > 0:
                bars_txt = "%d / %d BARS" % (min(done + 1, target), target)
            else:
                bars_txt = "BAR %d" % (done + 1)
        if rec and n >= 2:
            k = hh * min(4.0, 120.0 / max(1, max(rec[3]), -min(rec[2]))) / 127.0
            step = max(1, int(n / max(16.0, bw * n / PK_COLS / 2.0)))
            top, bot = [], []
            for a in range(0, n, step):
                e = min(n, a + step)
                x = x0 + bw * (a + e) / 2.0 / PK_COLS
                top += [x, cy - max(0.5, max(rec[3][a:e]) * k)]
                bot += [x, cy - min(-0.5, min(rec[2][a:e]) * k)]
            botrev = []
            for j in range(len(bot) // 2 - 1, -1, -1):
                botrev += bot[2 * j:2 * j + 2]
            c.create_polygon(top + botrev, fill=color, outline="")
            xe = x0 + bw * n / PK_COLS
            c.create_line(xe, cy - hh, xe, cy + hh, fill=TEXT, width=2)       # голова записи
        txt = bars_txt or "REC %.1f s" % tr["len"]
        self._text(x1 - 6, cy - hh * 0.62, txt, bw * 0.3, u * 0.26, TEXT, "e")

    def _waves(self, i, x0, x1, cy, hh, color, pos, reversed_):
        """Волна дорожки — картинкой: столбцы пикселей считаются только при изменении пиков (во время
        овердаба — только изменившиеся столбцы), а на кадре Tk лишь копирует готовую картинку.
        Многоугольники волн перерисовывались бы целиком при каждом сдвиге линии позиции.
        Сумма — цветом дорожки, слои овердабов и текущий овердаб — полупрозрачно поверх (смесь цветов)."""
        d = self._read_peaks(i)
        if not d:
            return
        W, H = max(2, int(x1 - x0)), max(2, int(2 * hh))
        g = self.geo.get(i)
        if not g or g["size"] != (W, H):
            g = {"size": (W, H), "img": tkinter.PhotoImage(width=W, height=H), "cols": None, "key": None}
            self.geo[i] = g
        key = (d["seq"], d["live"], color)
        if g["key"] != key:
            cols = self._wave_columns(d, W, H, color)
            old = g["cols"]
            if old is None or len(old) != W or g["key"][2] != color:
                rows = zip(*cols)
                g["img"].put(" ".join("{" + " ".join(r) + "}" for r in rows))
            else:  # дорос овердаб: только изменившиеся столбцы, подряд идущие — одним куском
                x = 0
                while x < W:
                    if cols[x] == old[x]:
                        x += 1
                        continue
                    e = x
                    while e < W and cols[e] != old[e]:
                        e += 1
                    rows = zip(*cols[x:e])
                    g["img"].put(" ".join("{" + " ".join(r) + "}" for r in rows), to=(x, 0))
                    x = e
            g["cols"], g["key"] = cols, key
        self.cv.create_image(int(x0), int(cy) - H // 2, image=g["img"], anchor="nw")

    def _wave_columns(self, d, W, H, color):
        """Цвета пикселей по столбцам: для каждого столбца список из H цветов сверху вниз."""
        n = PK_COLS
        k = (H / 2.0) * d["gain"] / 127.0
        mid = H / 2.0

        def spans(mn, mx):
            out = []
            for x in range(W):
                a = x * n // W
                e = max(a + 1, (x + 1) * n // W)
                hi, lo = max(mx[a:e]), min(mn[a:e])
                yt = int(max(0, mid - hi * k))
                yb = int(min(H, mid - lo * k + 1))
                if yb - yt < 1:
                    yt, yb = int(mid), int(mid) + 1
                out.append((yt, yb))
            return out

        base = spans(d["sum"][0], d["sum"][1])
        over = [(mix(LAYER_COLORS[no % len(LAYER_COLORS)], color, 0.5),
                 mix(LAYER_COLORS[no % len(LAYER_COLORS)], BG, 0.5), spans(mn, mx))
                for (no, mn, mx) in reversed(d["layers"])]
        if d["dub"]:
            no = (d["layers"][0][0] + 1) if d["layers"] else 1
            c = LAYER_COLORS[no % len(LAYER_COLORS)]
            over.append((mix(c, color, 0.5), mix(c, BG, 0.5), spans(d["dub"][0], d["dub"][1])))
        cols = []
        for x in range(W):
            yt, yb = base[x]
            col = [BG] * H
            col[yt:yb] = [color] * (yb - yt)
            for on_wave, on_bg, sp in over:
                lt, lb = sp[x]
                if lb - lt <= 1:
                    continue  # тишина в слое
                col[lt:lb] = [on_bg] * (lb - lt)
                a, b = max(lt, yt), min(lb, yb)
                if b > a:
                    col[a:b] = [on_wave] * (b - a)
            cols.append(col)
        return cols

    def _mixer(self, st, x0, y0, x1, y1):
        """Mixer по образцу Aeros: дорожки играющего парта (2, 4 или 6), справа Dry и Master."""
        c = self.cv
        m = self.monitors or {}
        w, hh = x1 - x0, y1 - y0
        side_w = max(70, w * 0.24)
        ng = st["n"]
        cw = (w - side_w - 6) / float(ng)
        ew = min(cw, (w - side_w - 6) / 4.0)   # ширина ручек: в 2×2 как в 4×4, по центру полосы
        base = st["part"] * ng
        for i in range(ng):
            c.section("mix%d" % i)
            tr = st["tracks"][i]
            n = base + i + 1
            cx0 = x0 + i * cw
            mid = cx0 + cw / 2
            ex0 = mid - ew / 2
            active = i == st["track"]
            col = track_color(tr["state"], tr["muted"], tr["reversed"], active)
            has_loop = tr["state"] in (ST_PLAY, ST_DUB)
            if i:
                c.create_line(cx0, y0, cx0, y1, fill="#1c1c1c", width=1)
            # круг позиции с номером
            r = min(ew * 0.16, hh * 0.075)
            self._pie(mid, y0 + r + 3, r, col, tr["pos"] if has_loop else 0.0, empty=not has_loop)
            self._text(mid, y0 + r + 3, str(i + 1), r * 1.3, r * 1.3, TEXT if has_loop else col)
            # панорама: полоса, метка центра, бегунок
            pan = self._zval("pan%d" % n)
            py0 = y0 + 2 * r + hh * 0.05
            ph = max(10, hh * 0.07)
            px0, px1 = ex0 + ew * 0.12, ex0 + ew * 0.88
            c.create_rectangle(px0, py0, px1, py0 + ph, fill="#1a1a1a", width=0)
            c.create_line((px0 + px1) / 2, py0, (px0 + px1) / 2, py0 + ph, fill=DIM)
            kx = (px0 + px1) / 2 + (px1 - px0) / 2 * pan / 100.0
            c.create_rectangle(kx - 3, py0 - 1, kx + 3, py0 + ph + 1, fill=TEXT, width=0)
            self.hits.append((px0 - 4, py0 - 6, px1 + 4, py0 + ph + 6, "pan", "pan%d" % n))
            # кнопки: режим канала и M
            by0 = py0 + ph + hh * 0.04
            bh = max(14, hh * 0.09)
            bw = (ew * 0.76 - 4) / 2
            chm = self._zval("chm%d" % n)  # 0 стерео, 1 только L, 2 только R
            split = chm in (1, 2)
            bx = ex0 + ew * 0.12
            c.create_rectangle(bx, by0, bx + bw, by0 + bh, fill="#3a2a5a" if split else "#262626", width=0)
            self._text(bx + bw / 2, by0 + bh / 2, ("ST", "L", "R")[chm if split else 0], bw * 0.9, bh * 0.7,
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
            self._fader(mid, fy0, fy1, ew, db, float(m.get("o_pk%d" % (i + 1), 0.0)), None, "lev%d" % n)
            self._text(mid, y1 - lab_h / 2 - 2, "-\u221e" if db <= MIN_DB else "%+.1f" % db, ew * 0.8, lab_h, DIM,
                       bold=False)
        # справа: Dry и Master
        c.section("mixside")
        sx0 = x1 - side_w
        c.create_line(sx0 - 3, y0, sx0 - 3, y1, fill="#2a2a2a", width=1)
        half = side_w / 2.0
        lab_h = max(10, hh * 0.07)
        top_h = max(14, hh * 0.09)
        for k, (sym, label) in enumerate((("dry", "DRY"), ("mst_db", "MASTER"))):
            mid = sx0 + half * (k + 0.5)
            self._text(mid, y0 + top_h / 2 + 2, label, half * 0.9, top_h * 0.8, TEXT)
            db = self._zfloat(sym)
            if sym == "mst_db":
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
        c = self.cv
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

    def _speed_view(self, st, x0, y0, x1, y1):
        """Скорость дорожки как у ленты: крупное значение, слайдер 0,25–2× с полутонами, 1×, привязка, ход."""
        c = self.cv
        c.section("speed")
        slot = self.speed_slot
        n = st["part"] * st["n"] + slot + 1
        sym = "spd%d" % n
        snap = self._zval("spd_snap")
        sp = speed_snap(self._zfloat(sym, 1.0), snap)
        st_val = 12.0 * math.log2(sp) if sp > 0 else 0.0
        tr = st["tracks"][slot]
        w, hh = x1 - x0, y1 - y0
        pad = max(10, w * 0.04)
        # значение (двойной тап — сброс к 1×)
        vh = hh * 0.3
        col = ORANGE if abs(sp - 1.0) > 1e-3 else TEXT
        self._text(x0 + w * 0.3, y0 + vh / 2 + 4, "%.2f×" % sp, w * 0.45, vh, col)
        self._text(x0 + w * 0.72, y0 + vh * 0.38 + 4, "%+.1f st" % st_val, w * 0.3, vh * 0.42, DIM)
        if abs(tr["espeed"] - tr["speed"]) > 1e-3:  # следование темпу поверх ручной
            self._text(x0 + w * 0.72, y0 + vh * 0.8 + 4, "tempo %.3f×" % tr["espeed"], w * 0.3, vh * 0.3, DIM,
                       bold=False)
        self.hits.append((x0, y0, x0 + w * 0.6, y0 + vh + 8, "sval", slot + 1))
        # слайдер
        sx0, sx1 = x0 + pad, x1 - pad
        sy = y0 + vh + hh * 0.22
        span = SPEED_MAX_ST - SPEED_MIN_ST
        c.create_line(sx0, sy, sx1, sy, fill="#3a3a3a", width=4)
        lab_h = max(10, hh * 0.08)
        for semi in range(int(SPEED_MIN_ST), int(SPEED_MAX_ST) + 1):
            x = sx0 + (sx1 - sx0) * (semi - SPEED_MIN_ST) / span
            octave = semi % 12 == 0
            th = hh * (0.07 if octave else 0.03)
            c.create_line(x, sy - th, x, sy + th, fill="#b0b0b0" if octave else "#4a4a4a", width=2 if octave else 1)
            if octave:  # крайние подписи — внутрь от края
                anc = "w" if semi == int(SPEED_MIN_ST) else ("e" if semi == int(SPEED_MAX_ST) else "center")
                lx = x - 4 if anc == "w" else (x + 4 if anc == "e" else x)
                self._text(lx, sy + th + lab_h * 0.7, "%g×" % (2.0 ** (semi / 12.0)), (sx1 - sx0) / 7.0, lab_h,
                           DIM, anc, bold=False)
        kx = sx0 + (sx1 - sx0) * (max(SPEED_MIN_ST, min(SPEED_MAX_ST, st_val)) - SPEED_MIN_ST) / span
        kh = hh * 0.09
        c.create_rectangle(kx - 7, sy - kh, kx + 7, sy + kh, fill=col, width=0)
        self.hits.append((sx0 - 10, sy - hh * 0.12, sx1 + 10, sy + hh * 0.12, "sslider", sym))
        # кнопки: 1×, привязка, плавный ход
        by0 = y1 - hh * 0.22
        bh = hh * 0.2
        gap = max(6, w * 0.02)
        bw = (w - 2 * pad - 2 * gap) / 3.0
        buttons = [("1×", "sreset", slot + 1, GREEN),
                   ("SNAP " + SNAP_NAMES[snap % 3], "snap", None, TEXT),
                   ("GLIDE %d ms" % self._zval("spd_glide"), "sglide", None, TEXT)]
        for k, (label, kind, arg, colr) in enumerate(buttons):
            bx = x0 + pad + k * (bw + gap)
            c.create_rectangle(bx, by0, bx + bw, by0 + bh, fill="#202020", width=0)
            self._text(bx + bw / 2, by0 + bh / 2, label, bw * 0.9, bh * 0.6, colr)
            self.hits.append((bx, by0, bx + bw, by0 + bh, kind, arg))

    def _settings(self, x0, y0, x1, y1):
        """Настройки одной колонкой с разделами: выбор из 2-4 вариантов — сегменты, времена и длины —
        ползунки. Не влезает — прокрутка пальцем, справа полоса прокрутки."""
        c = self.cv
        c.section("settings")
        w, vh = x1 - x0, y1 - y0
        rh = int(min(max(vh * 0.15, 26), 54))      # строка
        hdr = int(rh * 0.62)                        # заголовок раздела
        pad = max(8, int(w * 0.02))
        sb_w = 4
        lab_x = x0 + pad + rh * 0.22
        cx0 = x0 + pad + w * 0.30                   # начало переключателя или ползунка
        cx1 = x1 - pad - sb_w - 4
        n = getattr(self, "cur_n", 4)
        # длины — только дорожкам текущей сетки
        sections = [(title, [it for it in items if not (it[1].startswith("len") and int(it[1][3:]) > n)])
                    for title, items in SETTINGS]
        total = sum(hdr + len(items) * rh for _, items in sections) + rh * 0.3
        self.s_max = max(0.0, total - vh)
        self.s_scroll = max(0.0, min(self.s_max, self.s_scroll))

        def visible(a, b):  # надписи только у строк, видных целиком: наполовину скрытых не читают
            return a >= y0 - 1 and b <= y1 + 1

        def hit(hx0, hy0, hx1, hy1, what, arg):
            hy0, hy1 = max(hy0, y0), min(hy1, y1)
            if hy1 - hy0 > 4:
                self.hits.append((hx0, hy0, hx1, hy1, what, arg))

        y = y0 - self.s_scroll
        now = time.monotonic()
        confirm = self.grid_confirm if self.grid_confirm and now < self.grid_confirm[2] else None
        for title, items in sections:
            if y + hdr > y0 and y < y1:
                ty = y + hdr * 0.58
                if visible(y, y + hdr):
                    f = self._fit(title, w * 0.4, hdr * 0.62, True)
                    if f is not None:
                        c.create_text(x0 + pad, ty, text=title, fill="#7d7d7d", anchor="w", font=f)
                        tx = x0 + pad + f.measure(title) + pad
                        c.create_line(tx, ty, cx1, ty, fill="#262626", width=1)
            y += hdr
            for item in items:
                ry0, ry1 = y, y + rh
                y += rh
                if ry1 <= y0 or ry0 >= y1:
                    continue
                kind, sym, label = item[:3]
                cy = (ry0 + ry1) / 2
                ih = rh * 0.68                      # высота переключателя и ползунка
                full = visible(ry0, ry1)
                v = self._zval(sym)
                if v != SETTING_DEFAULTS.get(sym, v):  # отличается от значения по умолчанию
                    r = max(2, rh * 0.07)
                    c.create_oval(x0 + pad - r, cy - r, x0 + pad + r, cy + r, fill=GREEN, width=0)
                if sym == "pool_mb":  # рядом с подписью — заполнение пула или «меняется»
                    mm = self.monitors or {}
                    if mm.get("o_pool_mb") is not None and int(mm.get("o_pool_mb", 0)) == 0:
                        label = "POOL ..."
                    else:
                        label = "POOL %d%%" % int(round(100 * float(mm.get("o_pool", 0.0))))
                if full:
                    self._text(lab_x, cy, label, cx0 - lab_x - pad, ih * 0.62, "#d8d8d8", "w")
                if kind == "seg":
                    names = item[3]
                    vals = item[4] if len(item) > 4 else list(range(len(names)))
                    v = min(range(len(vals)), key=lambda q: abs(vals[q] - v))  # номер выбранного варианта
                    n = len(names)
                    sg = 3
                    sw = (cx1 - cx0 - sg * (n - 1)) / n
                    for k, name in enumerate(names):
                        sx = cx0 + k * (sw + sg)
                        on = k == v
                        fill, edge, tcol = (mix(GREEN, BG, 0.5), GREEN, TEXT) if on else ("#161616", "#2a2a2a", "#8a8a8a")
                        if confirm and confirm[0] == sym and confirm[1] == k:  # ждёт второго касания: стереть всё
                            fill = RED if int(now * 4) % 2 == 0 else mix(RED, BG, 0.5)
                            edge, tcol, name = RED, TEXT, "CLEAR ALL?"
                        c.create_rectangle(sx, cy - ih / 2, sx + sw, cy + ih / 2, fill=fill, outline=edge, width=1)
                        if full:
                            self._text(sx + sw / 2, cy, name, sw * 0.88, ih * 0.6, tcol)
                        hit(sx, ry0, sx + sw, ry1, "sseg", (sym, k, vals[k]))
                else:
                    values, fmt = item[3], item[4]
                    n = len(values)
                    i = min(range(n), key=lambda k: abs(values[k] - v))
                    try:
                        text = fmt(v)
                    except Exception:
                        text = str(v)
                    val_w = (cx1 - cx0) * 0.24
                    kr = ih * 0.32                  # бегунок
                    tx0, tx1 = cx0 + kr, cx1 - val_w - kr - pad * 0.5
                    kx = tx0 + (tx1 - tx0) * i / max(1, n - 1)
                    th = max(3, ih * 0.12)
                    c.create_rectangle(tx0, cy - th / 2, tx1, cy + th / 2, fill="#2a2a2a", width=0)
                    c.create_rectangle(tx0, cy - th / 2, kx, cy + th / 2, fill=mix(GREEN, BG, 0.35), width=0)
                    if n <= 17:  # шаги видны точками
                        dr = max(1.5, th * 0.35)
                        for k in range(n):
                            dx = tx0 + (tx1 - tx0) * k / max(1, n - 1)
                            c.create_oval(dx - dr, cy + th + dr, dx + dr, cy + th + 3 * dr,
                                          fill="#4a4a4a" if k > i else "#7aa77f", width=0)
                    c.create_oval(kx - kr, cy - kr, kx + kr, cy + kr, fill=TEXT, outline=GREEN, width=2)
                    if full:
                        self._text(cx1, cy, text, val_w, ih * 0.62, TEXT, "e")
                    hit(cx0 - pad * 0.5, ry0, cx1, ry1, "ssl", (sym, values, tx0, tx1))
                if ry1 < y1:
                    c.create_line(x0 + pad, ry1, cx1, ry1, fill="#141414", width=1)
        # полоса прокрутки
        if self.s_max > 0:
            sx = x1 - pad / 2 - sb_w
            c.create_rectangle(sx, y0 + 2, sx + sb_w, y1 - 2, fill="#1a1a1a", width=0)
            th = max(16, (vh - 4) * vh / total)
            ty = y0 + 2 + (vh - 4 - th) * self.s_scroll / self.s_max
            c.create_rectangle(sx, ty, sx + sb_w, ty + th, fill="#6a6a6a", width=0)
        self.hits.append((x0, y0, x1, y1, "sbody", None))  # остальное поле — прокрутка

    def _tabs(self, st, x0, y0, x1, y1, now):
        """Три вкладки снизу, как у Aeros: парт, дорожка и что сделает следующее нажатие Rec."""
        c = self.cv
        c.section("tabs")
        gap = 4
        tw = (x1 - x0 - gap * 2) / 3.0
        if st["rec_next"] and st["pend"] >= 0:
            hint, hcol = "REC \u2192 PART %d" % (st["pend"] + 1), RED
        elif now < self.action_until and st["act"]:
            hint, hcol = ACTIONS[st["act"]]
        else:
            hint, hcol = self._rec_hint(st)
        if self.speed_slot is not None:
            items = [("SPEED", TEXT), ("TRACK %d" % (self.speed_slot + 1), GREEN), ("CLOSE", GREEN)]
        elif self.settings_open:
            if self.grid_confirm and now < self.grid_confirm[2]:
                mid_item = ("TAP AGAIN TO CLEAR", RED)
            else:
                mid_item = ("SWIPE \u2195 TO SCROLL" if self.s_max > 0 else "TAP TO CHANGE", DIM)
            items = [("SETTINGS", TEXT), mid_item, ("CLOSE", GREEN)]
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
        if self.speed_slot is not None:
            self.hits.append((x0 + 2 * (tw + gap), y0, x1, y1, "sclose", None))
        elif self.settings_open:
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
