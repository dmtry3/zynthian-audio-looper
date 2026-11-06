#!/usr/bin/python3
# Виджет Audio Looper без Zynthian, с заглушками модулей zyngui (по образцу generative-arp).
#   python3 tests/widget_preview.py          сценарий с проверками (PASS/FAIL)
#   python3 tests/widget_preview.py --show   окно 800x480: темп идёт, действия меняются сами
import importlib.util
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
                  "o_pend": -1.0, "o_widget": 0.0}

    def get_monitors_dict(self):
        return dict(self.m)


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1


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


def texts(wdg):
    c = wdg.widget_canvas
    return [c.itemcget(i, "text") for i in c.find_all() if c.type(i) == "text"]


def main():
    show = "--show" in sys.argv
    mod = load_widget()
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

    check("виджет растянут на окно", wdg.width >= 700 and wdg.height >= 400)
    check("темп и счёт видны", "120.0 BPM" in texts(wdg) and "1.1" in texts(wdg))

    m["o_action"], m["o_act_cnt"] = 1.0, 1.0
    wdg.update()
    rect_fills = [wdg.widget_canvas.itemcget(i, "fill") for i in wdg.widget_canvas.find_all()
                  if wdg.widget_canvas.type(i) == "rectangle"]
    check("действие Rec красит прямоугольник красным", "#e03030" in rect_fills and "REC / DUB" in texts(wdg))

    m["o_bar"], m["o_beat"] = 1.0, 0.05
    wdg.update()
    check("смена такта даёт вспышку", wdg.flash_until > time.monotonic())
    check("счёт 2.1", "2.1" in texts(wdg))

    m["o_bpb"], m["o_beat"] = 3.0, 2.5
    wdg.update()
    ovals = [i for i in wdg.widget_canvas.find_all() if wdg.widget_canvas.type(i) == "oval"]
    check("точек долей по числу долей в такте", len(ovals) == 3 and "2.3" in texts(wdg))

    m["o_tsrc"] = 0.0
    wdg.update()
    check("без темпа видно NO TEMPO", "NO TEMPO" in texts(wdg))

    m["o_widget"], m["o_track"], m["o_part"] = 1.0, 2.0, 3.0
    wdg.update()
    check("активный виджет Mixer и дорожка/парт", "MIXER" in texts(wdg) and "TRACK 3   PART 4" in texts(wdg))

    m["o_action"] = 99.0
    wdg.update()
    check("неизвестный код действия не ломает виджет", "-" in texts(wdg))

    root.destroy()
    print("%s" % ("OK" if not fails else "%d FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
