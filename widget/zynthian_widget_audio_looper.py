#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Audio Looper: заглушка виджета (этап 0).
# Показывает, что плагин видит кнопки футсвитча (цвет большого прямоугольника меняется
# по последнему действию) и темп Zynthian (полоса темпа сверху, сильная доля крупнее и ярче).
# Виджет только читает выходные порты плагина (self.monitors), ничего не ждёт.
import time
import tkinter

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

# Коды порта o_action, как enum Action в lv2/audio_looper.cpp: (подпись, цвет)
ACTIONS = [
    ("-", "#303030"),
    ("REC / DUB", "#e03030"),
    ("UNDO", "#e08a20"),
    ("REDO", "#e0b020"),
    ("RESET TRACK", "#808080"),
    ("RESET ALL", "#b0b0b0"),
    ("MUTE", "#606060"),
    ("REVERSE", "#9050d0"),
    ("LOCK", "#d0c030"),
    ("NEXT TRACK", "#30b050"),
    ("PREV TRACK", "#208040"),
    ("NEXT PART", "#3070e0"),
    ("PREV PART", "#2050a0"),
    ("PLAY / STOP", "#20b0b0"),
    ("WIDGET LOOP", "#d0d0d0"),
    ("WIDGET MIXER", "#d0d0d0"),
]
# Биты o_held в порядке контроллеров-действий
BUTTONS = ["Rec", "Undo", "Redo", "Rst", "Mute", "Rev", "Lock", "T+",
           "T-", "P+", "P-", "Play", "Loop", "Mix"]
TEMPO_SRC = ["NO TEMPO", "time:Position", "MIDI clock"]
FLASH_S = 0.12          # сколько длится вспышка полосы на сильной доле


def dim(col, f):
    r, g, b = int(col[1:3], 16), int(col[3:5], 16), int(col[5:7], 16)
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


class zynthian_widget_audio_looper(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.last_bar = None
        self.flash_until = 0.0
        self.last_cnt = None
        self.pulse_until = 0.0

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.refresh_gui()

    def _font(self, size, bold=False):
        fam = zynthian_gui_config.font_family
        return (fam, max(6, int(size)), "bold") if bold else (fam, max(6, int(size)))

    def refresh_gui(self):
        m = self.monitors or {}
        c = self.widget_canvas
        c.delete("all")
        w, h = max(self.width, 40), max(self.height, 40)
        now = time.monotonic()

        bpm = float(m.get("o_bpm", 0.0))
        bpb = max(1, int(round(m.get("o_bpb", 4))))
        bar = int(m.get("o_bar", 0))
        beat = float(m.get("o_beat", 0.0))
        rolling = int(m.get("o_rolling", 0))
        src = int(m.get("o_tsrc", 0))
        clk_bpm = float(m.get("o_clk_bpm", 0.0))
        pos_cnt = int(m.get("o_pos_cnt", 0))
        act = int(m.get("o_action", 0))
        cnt = int(m.get("o_act_cnt", 0))
        held = int(m.get("o_held", 0))
        track = int(m.get("o_track", 0))
        part = int(m.get("o_part", 0))
        wmode = int(m.get("o_widget", 0))
        if act < 0 or act >= len(ACTIONS):
            act = 0
        beat_i = min(bpb - 1, max(0, int(beat)))

        # Вспышка на сильной доле: по смене такта из аудиопотока
        if self.last_bar is not None and bar != self.last_bar and src:
            self.flash_until = now + FLASH_S
        self.last_bar = bar
        # Новое действие подсвечивает рамку
        if self.last_cnt is not None and cnt != self.last_cnt:
            self.pulse_until = now + 0.25
        self.last_cnt = cnt

        # ---------- полоса темпа ----------
        th = int(h * 0.2)
        flash = now < self.flash_until
        c.create_rectangle(0, 0, w, th, fill="#505050" if flash else "#181818", width=0)
        fs = th * 0.45
        if src:
            c.create_text(8, th / 2, anchor="w", fill="#ffffff", font=self._font(fs, True),
                          text="%.1f BPM" % bpm)
            c.create_text(w * 0.36, th / 2, anchor="w", fill="#ffffff", font=self._font(fs, True),
                          text="%d.%d" % (bar + 1, beat_i + 1))
        else:
            c.create_text(8, th / 2, anchor="w", fill="#ff5050", font=self._font(fs, True),
                          text=TEMPO_SRC[0])
        # точки долей: первая крупнее, текущая яркая
        x0, x1 = w * 0.52, w - 10
        step = (x1 - x0) / bpb
        for i in range(bpb):
            r = th * (0.3 if i == 0 else 0.18)
            on = (i == beat_i) and src
            col = "#ffffff" if on else ("#707070" if i == 0 else "#404040")
            cx = x0 + step * (i + 0.5)
            c.create_oval(cx - r, th / 2 - r, cx + r, th / 2 + r, fill=col, width=0)

        # ---------- большой прямоугольник: последнее действие ----------
        label, col = ACTIONS[act]
        top, bot = th + 8, int(h * 0.74)
        pulse = now < self.pulse_until
        c.create_rectangle(8, top, w - 8, bot, fill=col,
                           outline="#ffffff" if pulse else dim(col, 0.6), width=4 if pulse else 2)
        c.create_text(w / 2, (top + bot) / 2 - (bot - top) * 0.1, fill="#000000" if act else "#808080",
                      font=self._font((bot - top) * 0.28, True), text=label)
        c.create_text(w / 2, bot - (bot - top) * 0.14, fill="#000000" if act else "#808080",
                      font=self._font((bot - top) * 0.11), text="#%d" % cnt)
        c.create_text(16, top + 6, anchor="nw", fill="#000000" if act else "#a0a0a0",
                      font=self._font((bot - top) * 0.12, True),
                      text="MIXER" if wmode == 1 else "LOOP")

        # ---------- нижняя строка: дорожка, парт, источник темпа ----------
        y = bot + (h - bot) * 0.3
        fi = (h - bot) * 0.22
        c.create_text(10, y, anchor="w", fill="#30d050", font=self._font(fi, True),
                      text="TRACK %d   PART %d" % (track + 1, part + 1))
        info = "%s  msgs %d  %s" % (TEMPO_SRC[src] if 0 <= src < 3 else "?", pos_cnt,
                                    "RUN" if rolling else "STOP")
        if clk_bpm > 0:
            info += "  clk %.1f" % clk_bpm
        c.create_text(w - 10, y, anchor="e", fill="#a0a0a0", font=self._font(fi * 0.8), text=info)

        # нажатые сейчас кнопки: видно, приходит ли отпускание (momentary)
        y2 = bot + (h - bot) * 0.75
        bw = (w - 20) / len(BUTTONS)
        for i, name in enumerate(BUTTONS):
            down = (held >> i) & 1
            bx = 10 + i * bw
            c.create_rectangle(bx + 2, y2 - fi * 0.8, bx + bw - 2, y2 + fi * 0.8,
                               fill="#e0e0e0" if down else "#202020", outline="#505050")
            c.create_text(bx + bw / 2, y2, fill="#000000" if down else "#808080",
                          font=self._font(fi * 0.6), text=name)
