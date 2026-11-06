#!/usr/bin/python3
# Проверка lv2/audio_looper.ttl под правила Zynthian (запускается из make test):
#  - входные порты-контроллеры занимают номера 0..N-1: Zynthian нумерует LV2-параметры с N,
#    и порт-контроллер с номером >= N затёрся бы параметром (так однажды пропала страница Mix);
#  - символы портов и параметров не повторяются: Zynthian хранит контроллеры по символу.
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ttl = open(os.path.join(ROOT, "lv2", "audio_looper.ttl"), encoding="utf-8").read()

fails = 0


def check(name, cond, extra=""):
    global fails
    print("%s: %s%s" % ("PASS" if cond else "FAIL", name, (" — " + extra) if extra and not cond else ""))
    fails += 0 if cond else 1


ports = []
for block in re.findall(r"\[\s*(a lv2:\w+Port.*?)\]", ttl, re.S):
    idx = re.search(r"lv2:index (\d+)", block)
    sym = re.search(r'lv2:symbol "(\w+)"', block)
    if idx and sym:
        ports.append((int(idx.group(1)), sym.group(1), "lv2:InputPort" in block and "lv2:ControlPort" in block))
params = re.findall(r'a lv2:Parameter ;\s*lv2:symbol "(\w+)"', ttl)

ctrl_in = sorted(i for i, _, is_ctrl_in in ports if is_ctrl_in)
check("входные контроллеры занимают номера 0..%d" % (len(ctrl_in) - 1), ctrl_in == list(range(len(ctrl_in))),
      str(ctrl_in))
indices = [i for i, _, _ in ports]
check("номера портов не повторяются и идут подряд", sorted(indices) == list(range(len(indices))))
syms = [s for _, s, _ in ports] + params
dups = sorted({s for s in syms if syms.count(s) > 1})
check("символы портов и параметров не повторяются", not dups, ", ".join(dups))
check("параметры объявлены в patch:writable", all("#%s>" % p in ttl.split("lv2:port")[0] for p in params))
print("ttl: %s" % ("OK" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
