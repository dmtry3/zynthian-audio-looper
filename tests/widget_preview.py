#!/usr/bin/python3
# Виджет Audio Looper без Zynthian, с заглушками модулей zyngui (по образцу generative-arp).
#   python3 tests/widget_preview.py          сценарий с проверками (PASS/FAIL)
#   python3 tests/widget_preview.py --show   окно 800x480: темп идёт, действия меняются сами
import importlib.util
import math
import os
import sys
import time
import tkinter
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------- заглушки zyngui ----------
cfg = types.ModuleType("zyngui.zynthian_gui_config")
cfg.font_family = "DejaVu Sans"
cfg.color_bg = "#000000"
cfg.zyngui = None


class _WidgetBase(tkinter.Frame):          # тот же контракт, что у zynthian_widget_base
    def __init__(self, parent):
        super().__init__(parent.main_frame, bg=cfg.color_bg)
        self.zyngui = cfg.zyngui
        self.zyngui_control = parent
        self.width = 1
        self.height = 1
        self.rows = parent.layout['rows']
        self.processor = None
        self.widget_canvas = None
        self.monitors = None
        self.bind('<Configure>', self.on_size)

    def on_size(self, event):
        if event.width == self.width and event.height == self.height:
            return False
        self.width, self.height = event.width, event.height
        try:
            self.widget_canvas.configure(width=self.width, height=self.height)
        except Exception:
            pass
        return True

    def update(self):
        self.monitors = self.processor.engine.get_monitors_dict()
        self.refresh_gui()

    def set_processor(self, processor):
        self.processor = processor


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = _WidgetBase
pkg = types.ModuleType("zyngui")
pkg.zynthian_gui_config, pkg.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": pkg, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})


class Engine:
    def __init__(self):
        self.m = {"o_bpm": 120.0, "o_bpb": 4.0, "o_bar": 0.0, "o_beat": 0.0, "o_rolling": 1.0,
                  "o_tsrc": 1.0, "o_clk_bpm": 0.0, "o_pos_cnt": 1.0, "o_action": 0.0,
                  "o_act_cnt": 0.0, "o_held": 0.0, "o_track": 0.0, "o_part": 0.0,
                  "o_pend": -1.0, "o_widget": 0.0, "o_flags": 0.0, "o_pool": 0.0, "o_parts": 0.0}
        for i in range(1, 5):
            self.m["o_st%d" % i] = 0.0
            self.m["o_pos%d" % i] = 0.0
            self.m["o_len%d" % i] = 0.0

    def get_monitors_dict(self):
        return dict(self.m)


class Ctrl:
    def __init__(self, sym, value, lo, hi):
        self.symbol, self.value, self.lo, self.hi = sym, value, lo, hi

    def set_value(self, v, send=True):
        assert isinstance(v, (int, float)), "%s: %r is not a number" % (self.symbol, v)
        assert self.lo <= v <= self.hi, "%s: %r out of range" % (self.symbol, v)
        self.value = v
        self.writes = getattr(self, "writes", 0) + 1


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        # настройки — LV2-параметры плагина (как в lv2/audio_looper.ttl)
        self.controllers_dict = {
            "rec_order": Ctrl("rec_order", 0, 0, 1), "mode": Ctrl("mode", 1, 0, 1),
            "fade_out": Ctrl("fade_out", 200, 10, 4000), "fade_in": Ctrl("fade_in", 200, 10, 4000),
            "rev_time": Ctrl("rev_time", 50, 5, 1000), "mute_q": Ctrl("mute_q", 2, 0, 3),
            "part_q": Ctrl("part_q", 3, 0, 3),
        }
        for i in range(1, 5):
            self.controllers_dict["len%d" % i] = Ctrl("len%d" % i, 0, 0, 16)
        # микшер (LV2-параметры): 16 дорожек, Dry, Master, ограничитель, команда mute
        for i in range(1, 17):
            self.controllers_dict["lev%d" % i] = Ctrl("lev%d" % i, 0.0, -60.0, 6.0)
            self.controllers_dict["pan%d" % i] = Ctrl("pan%d" % i, 0, -100, 100)
            self.controllers_dict["chm%d" % i] = Ctrl("chm%d" % i, 0, 0, 2)
        self.controllers_dict["dry"] = Ctrl("dry", 0.0, -60.0, 6.0)
        self.controllers_dict["mst_db"] = Ctrl("mst_db", 0.0, -60.0, 6.0)
        self.controllers_dict["limiter"] = Ctrl("limiter", 1, 0, 1)
        self.controllers_dict["tmute"] = Ctrl("tmute", 0, 0, 4)
        # скорость (этап 5)
        for i in range(1, 17):
            self.controllers_dict["spd%d" % i] = Ctrl("spd%d" % i, 1.0, 0.25, 2.0)
        self.controllers_dict["sreset"] = Ctrl("sreset", 0, 0, 4)
        self.controllers_dict["spd_snap"] = Ctrl("spd_snap", 0, 0, 2)
        self.controllers_dict["spd_glide"] = Ctrl("spd_glide", 150, 0, 2000)
        self.controllers_dict["tempo_glide"] = Ctrl("tempo_glide", 500, 100, 4000)


class Control:
    def __init__(self, root):
        self.main_frame = root
        self.layout = {"rows": 4}


def load_widget():
    path = os.path.join(ROOT, "widget", "zynthian_widget_audio_looper.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_audio_looper", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_fake_peaks(path):
    """Подставные пики в раскладке PeakShm: дорожка 1 — основа и 3 слоя, 2 — идёт овердаб, 3 — основа."""
    import math
    import struct as st
    cols, nlay = 512, 8
    track = 16 + 4 * nlay + 2 * cols * (2 + nlay)
    buf = bytearray(16 + 4 * track)
    st.pack_into("<4I", buf, 0, 0x31504C41, cols, 4, nlay)

    def wave(f, amp, a=0, b=cols, phase=0.0):
        mn, mx = [0] * cols, [0] * cols
        for c in range(a, b):
            env = amp * (0.55 + 0.45 * math.sin(c / cols * math.pi * f + phase)) * (0.7 + 0.3 * math.sin(c * 0.9))
            v = int(max(1, min(127, env * 127)))
            mx[c], mn[c] = v, -v
        return mn, mx

    def put(off, mn, mx):
        st.pack_into("%db" % cols, buf, off, *mn)
        st.pack_into("%db" % cols, buf, off + cols, *mx)

    def track_off(i):
        return 16 + i * track

    sum_off, dub_off = 16 + 4 * nlay, 16 + 4 * nlay + 2 * cols
    lay_off = dub_off + 2 * cols
    # дорожка 1: основа + 3 слоя
    o = track_off(0)
    st.pack_into("<4I", buf, o, 2, 0, 3, 0)
    st.pack_into("<3I", buf, o + 16, 3, 2, 1)
    put(o + sum_off, *wave(8, 0.75))
    put(o + lay_off, *wave(16, 0.35, 300, 512))
    put(o + lay_off + 2 * cols, *wave(6, 0.3, 120, 330, 1.0))
    put(o + lay_off + 4 * cols, *wave(4, 0.25, 0, 200, 2.0))
    # дорожка 2: идёт овердаб
    o = track_off(1)
    st.pack_into("<4I", buf, o, 2, 5, 0, 1)
    put(o + sum_off, *wave(12, 0.6))
    put(o + dub_off, *wave(20, 0.3, 0, 280))
    # дорожка 3: только основа
    o = track_off(2)
    st.pack_into("<4I", buf, o, 2, 0, 0, 0)
    put(o + sum_off, *wave(5, 0.9))
    with open(path, "wb") as f:
        f.write(buf)


def texts(wdg):
    c = wdg.widget_canvas
    return [c.itemcget(i, "text") for i in c.find_all() if c.type(i) == "text"]


def main():
    show = "--show" in sys.argv
    mod = load_widget()
    os.makedirs(os.path.join(ROOT, "tests", "out"), exist_ok=True)
    mod.PEAKS_PATH_OVERRIDE = os.path.join(ROOT, "tests", "out", "peaks.bin")
    write_fake_peaks(mod.PEAKS_PATH_OVERRIDE)
    root = tkinter.Tk()
    root.geometry("800x480")
    proc = Processor()
    wdg = mod.zynthian_widget_audio_looper(Control(root))
    wdg.set_processor(proc)
    wdg.pack(fill="both", expand=True)
    root.update()
    wdg.update()
    root.update()
    m = proc.engine.m

    if show:
        t0 = time.monotonic()

        def tick():
            beats = (time.monotonic() - t0) * m["o_bpm"] / 60.0
            m["o_bar"], m["o_beat"] = float(int(beats // 4)), beats % 4
            k = int(beats // 2) % 15 + 1
            if k != int(m["o_action"]):
                m["o_action"], m["o_act_cnt"] = float(k), m["o_act_cnt"] + 1
            wdg.update()
            root.after(33, tick)
        tick()
        root.mainloop()
        return 0

    fails = 0

    def check(name, cond):
        nonlocal fails
        print("%s: %s" % ("PASS" if cond else "FAIL", name))
        fails += 0 if cond else 1

    def boxes():
        c = wdg.widget_canvas
        return [(c.itemcget(i, "text"), c.bbox(i)) for i in c.find_all() if c.type(i) == "text"]

    def no_overlap(tag):
        bb = boxes()
        bad = []
        for a in range(len(bb)):
            ta, (ax0, ay0, ax1, ay1) = bb[a]
            if ax0 < -1 or ay0 < -1 or ax1 > wdg.width + 1 or ay1 > wdg.height + 1:
                bad.append("%r за краем" % ta)
            for b in range(a + 1, len(bb)):
                tb, (bx0, by0, bx1, by1) = bb[b]
                # bbox у tk с запасом в пару пикселей: считаем наезд, если пересечение больше 2 px
                if min(ax1, bx1) - max(ax0, bx0) > 2 and min(ay1, by1) - max(ay0, by0) > 2:
                    bad.append("%r и %r" % (ta, tb))
        check("надписи не наезжают (%s)" % tag, not bad)
        if bad:
            print("   ", "; ".join(bad[:6]))

    check("виджет растянут на окно", wdg.width >= 700 and wdg.height >= 400)
    check("темп и счёт видны", "120" in texts(wdg) and "1.1" in texts(wdg) and "4/4" in texts(wdg))
    no_overlap("800x480, Loop")

    m["o_action"], m["o_act_cnt"] = 1.0, 1.0
    wdg.update()
    fills = [wdg.widget_canvas.itemcget(i, "fill") for i in wdg.widget_canvas.find_all()]
    check("нажатие Rec горит красным во вкладке", mod.RED in fills and "REC / DUB" in texts(wdg))

    m["o_bar"], m["o_beat"] = 1.0, 0.05
    wdg.update()
    check("смена такта даёт вспышку", wdg.flash_until > time.monotonic())
    check("счёт 2.1", "2.1" in texts(wdg))

    m["o_bpb"], m["o_beat"] = 3.0, 2.5
    wdg.update()
    ovals = [i for i in wdg.widget_canvas.find_all() if wdg.widget_canvas.type(i) == "oval"]
    check("3 точки долей + 4 точки партов + 4 круга дорожек + шестерёнка", len(ovals) == 3 + 4 + 4 + 2 and "2.3" in texts(wdg) and "3/4" in texts(wdg))

    m["o_tsrc"] = 0.0
    wdg.update()
    check("без темпа видно NO TEMPO", "NO TEMPO" in texts(wdg))
    m["o_tsrc"] = 1.0

    m["o_widget"], m["o_track"] = 1.0, 2.0
    wdg.update()
    check("активный виджет Mixer: кнопки M и MASTER", texts(wdg).count("M") == 4 and "MASTER" in texts(wdg))
    no_overlap("800x480, Mixer")
    m["o_widget"] = 0.0

    m["o_action"], m["o_act_cnt"] = 99.0, 5.0
    wdg.update()
    check("неизвестный код действия не ломает виджет", "RECORD" in texts(wdg))

    # состояния дорожек: запись красная, овердаб оранжевый, mute серый, reverse фиолетовый
    m["o_st1"], m["o_st2"], m["o_st3"], m["o_st4"] = 2.0, 4.0, 3.0, 3.0
    m["o_len1"], m["o_len2"], m["o_len3"], m["o_len4"] = 1.5, 8.0, 8.0, 4.0
    m["o_pos2"], m["o_pos3"], m["o_pos4"] = 0.3, 0.6, 0.9
    m["o_flags"] = float((1 << 2) | (1 << 7))  # дорожка 3 mute, дорожка 4 reverse
    m["o_track"] = 3.0
    wdg.update()
    fills = [wdg.widget_canvas.itemcget(i, "fill") for i in wdg.widget_canvas.find_all()]
    check("цвета состояний дорожек", all(col in fills for col in (mod.RED, mod.ORANGE, mod.GRAY)))
    check("reverse фиолетовый", mod.PURPLE in fills)
    no_overlap("800x480, дорожки играют")
    polys = [i for i in wdg.widget_canvas.find_all() if wdg.widget_canvas.type(i) == "polygon"]
    stip = [i for i in polys if wdg.widget_canvas.itemcget(i, "stipple")]
    check("волны: сумма на играющих дорожках, овердаб полупрозрачно", len(stip) == 1 and len(polys) >= 3 + 1)
    m["o_st1"] = 3.0
    wdg.update()
    polys = [i for i in wdg.widget_canvas.find_all() if wdg.widget_canvas.type(i) == "polygon"]
    stip = [i for i in polys if wdg.widget_canvas.itemcget(i, "stipple")]
    check("слои овердабов дорожки 1 полупрозрачно поверх", len(stip) == 3 + 1)
    no_overlap("800x480, волны")
    m["o_st1"] = 2.0
    # парты и Lock: записаны 1 и 3, текущий 1, ждёт 2; дорожка 3 с замком
    m["o_parts"], m["o_part"], m["o_pend"] = 5.0, 0.0, 1.0
    m["o_flags"] = float(m["o_flags"]) + float(1 << 22)
    wdg.update()
    arcs = [i for i in wdg.widget_canvas.find_all() if wdg.widget_canvas.type(i) == "arc"
            and wdg.widget_canvas.itemcget(i, "style") == "arc"]
    check("значок замка на дорожке с Lock", len(arcs) == 1)
    no_overlap("800x480, парты и Lock")
    m["o_pend"] = -1.0
    m["o_track"] = 0.0
    m["o_st1"] = 0.0
    wdg.update()
    check("подсказка педали Rec на пустой дорожке", "RECORD" in texts(wdg))

    # шестерёнка открывает настройки, стрелки меняют значения
    gear = [hh for hh in wdg.hits if hh[4] == "gear"][0]
    wdg._on_press(types.SimpleNamespace(x=(gear[0] + gear[2]) / 2, y=(gear[1] + gear[3]) / 2))
    check("шестерёнка открывает настройки", wdg.settings_open and "QUANTIZE" in texts(wdg))
    no_overlap("800x480, настройки")

    def tap_step(sym, d):
        hit = [hh for hh in wdg.hits if hh[4] == "step" and hh[5][0] == sym and hh[5][2] == d][0]
        wdg._on_press(types.SimpleNamespace(x=(hit[0] + hit[2]) / 2, y=(hit[1] + hit[3]) / 2))

    tap_step("mode", -1)
    check("MODE: Quantize -> Free", proc.controllers_dict["mode"].value == 0 and "FREE" in texts(wdg))
    tap_step("mode", -1)
    check("на краю списка значение не уходит за предел", proc.controllers_dict["mode"].value == 0)
    tap_step("fade_out", 1)
    check("FADE OUT 200 -> 300 ms", proc.controllers_dict["fade_out"].value == 300 and "300 ms" in texts(wdg))
    tap_step("len2", 1)
    tap_step("len2", 1)
    check("LENGTH 2 -> 2 BARS", proc.controllers_dict["len2"].value == 2 and "2 BARS" in texts(wdg))
    m["o_widget"] = 1.0
    wdg.update()
    check("переключение Loop/Mixer с футсвитча закрывает настройки", not wdg.settings_open)
    m["o_widget"] = 0.0
    wdg.update()

    # Mixer: фейдер, двойной тап, панорама, режим канала, M, ограничитель
    m["o_widget"], m["o_part"], m["o_track"] = 1.0, 1.0, 0.0
    m["o_pk1"], m["o_mpk_l"], m["o_mpk_r"] = 0.5, 0.7, 0.6
    wdg.update()
    no_overlap("800x480, Mixer")
    cd = proc.controllers_dict

    def hit(kind, arg):
        return [hh for hh in wdg.hits if hh[4] == kind and hh[5] == arg][0]

    def tap(kind, arg, fx=0.5, fy=0.5):
        x0, y0, x1, y1 = hit(kind, arg)[:4]
        ev = types.SimpleNamespace(x=x0 + (x1 - x0) * fx, y=y0 + (y1 - y0) * fy)
        wdg._on_press(ev)
        wdg._on_release(ev)

    f = hit("fader", "lev5")  # парт 2, дорожка 1 = lev5
    wdg._on_press(types.SimpleNamespace(x=(f[0] + f[2]) / 2, y=f[1] + (f[3] - f[1]) * 0.6))
    wdg._on_motion(types.SimpleNamespace(x=(f[0] + f[2]) / 2, y=f[1] + (f[3] - f[1]) * 0.8))
    wdg._on_release(None)
    v = cd["lev5"].value
    check("фейдер дорожки тянется пальцем (ниже 0 дБ)", -40.0 < v < -3.0)
    time.sleep(0.4)
    tap("fader", "lev5", fy=0.9)
    tap("fader", "lev5", fy=0.9)
    check("двойной тап по фейдеру — 0 дБ", cd["lev5"].value == 0.0)
    time.sleep(0.4)
    tap("pan", "pan6", fx=0.9)
    check("панорама дорожки 2 вправо", cd["pan6"].value > 60)
    tap("chm", "chm7")
    check("режим канала дорожки 3: L", cd["chm7"].value == 1)
    wdg.update()
    check("кнопка L видна", "L" in texts(wdg))
    tap("chm", "chm7")
    check("режим канала дорожки 3: R", cd["chm7"].value == 2)
    tap("chm", "chm7")
    check("режим канала дорожки 3: снова ST", cd["chm7"].value == 0)
    tap("mute", 2)
    check("M дорожки 2 — команда tmute=2", cd["tmute"].value == 2)
    tap("lim", None)
    check("LIM выключает ограничитель", cd["limiter"].value == 0)
    tap("fader", "mst_db", fy=0.95)
    check("фейдер Master", cd["mst_db"].value < -20.0)
    no_overlap("800x480, Mixer после касаний")
    m["o_widget"], m["o_part"] = 0.0, 0.0
    wdg.update()

    # Speed: касание по метке скорости у дорожки открывает экран Speed
    m["o_widget"], m["o_part"], m["o_st2"], m["o_spd2"], m["o_espd2"] = 0.0, 0.0, 3.0, 0.84, 0.98
    wdg.update()
    check("метка скорости у дорожки 2 горит", "0.84\u00d7" in texts(wdg))
    tap("speed", 1)
    check("открылся экран Speed дорожки 2", wdg.speed_slot == 1 and "SPEED" in texts(wdg))
    no_overlap("800x480, Speed")
    tap("sslider", "spd2", fx=0.5)  # середина: −6 полутонов
    check("слайдер: −6 полутонов", abs(12 * math.log2(cd["spd2"].value) + 6.0) < 0.3)
    tap("sslider", "spd2", fx=2.0 / 3.0 + 0.002)  # почти 1×: прилипает
    check("у 1× слайдер прилипает", cd["spd2"].value == 1.0)
    tap("snap", None)
    check("привязка: SEMI", cd["spd_snap"].value == 1 and "SNAP SEMI" in texts(wdg))
    tap("sglide", None)
    check("плавный ход: следующее значение", cd["spd_glide"].value == 250)
    tap("sreset", 2)
    check("кнопка 1x — команда сброса дорожки 2", cd["sreset"].value == 2)
    cd["sreset"].value = 0
    tap("sval", 2)
    tap("sval", 2)
    check("двойной тап по значению — сброс", cd["sreset"].value == 2)
    tap("sclose", None)
    check("CLOSE закрывает Speed", wdg.speed_slot is None)

    # виджет в Zynthian занимает только часть экрана: проверяем и маленькие размеры
    m["o_bpm"], m["o_bar"], m["o_bpb"] = 133.5, 127.0, 7.0
    for size in ("640x360", "480x272", "360x220"):
        root.geometry(size)
        root.update()
        wdg.update()
        root.update()
        wdg.update()
        no_overlap(size)
        wdg.settings_open = True
        wdg.update()
        no_overlap(size + ", настройки")
        wdg.settings_open = False
        m["o_widget"] = 1.0
        wdg.update()
        no_overlap(size + ", Mixer")
        m["o_widget"] = 0.0
        wdg.speed_slot = 0
        wdg.update()
        no_overlap(size + ", Speed")
        wdg.speed_slot = None

    root.destroy()
    print("%s" % ("OK" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
