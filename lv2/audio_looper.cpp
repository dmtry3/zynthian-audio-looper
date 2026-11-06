// Audio Looper: тонкая LV2-обёртка над ядром из core/.
// Порты и контроллеры-действия (тап и удержание), темп из time:Position (запасной путь — MIDI-клок),
// настройки как LV2-параметры (patch:Set, сохраняются через state), undo/redo через LV2 worker.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "../core/gestures.h"
#include "../core/looper.h"
#include "../core/tempo.h"

#define AL_URI "urn:audiolooper:looper"

namespace {

using namespace alp;

constexpr size_t kPoolBytes = (size_t)2 << 30;  // 2 ГБ по ТЗ (настройка размера — этап 6)

// Порядок совпадает с lv2:index в audio_looper.ttl.
enum Port : uint32_t {
  P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_EVENTS,
  // страницы 1–4: контроллеры-действия
  P_REC, P_UNDO, P_REDO, P_RESET,
  P_MUTE, P_REVERSE, P_LOCK, P_TRK_NEXT,
  P_TRK_PREV, P_PART_NEXT, P_PART_PREV, P_PLAY,
  P_W_LOOP, P_W_MIXER,
  // страница 5: плавные (уровень, панорама, master, скорость — этапы 4 и 5)
  P_LEVEL, P_PAN, P_MASTER, P_SPEED,
  // выходы только для чтения
  O_TRACK, O_PART, O_PEND, O_WIDGET, O_ACTION, O_ACT_CNT, O_HELD,
  O_BPM, O_BAR, O_BEAT, O_BPB, O_ROLLING, O_TSRC, O_CLK_BPM, O_POS_CNT,
  P_EVENTS_OUT,
  O_ST1, O_ST2, O_ST3, O_ST4,
  O_POS1, O_POS2, O_POS3, O_POS4,
  O_LEN1, O_LEN2, O_LEN3, O_LEN4,
  O_FLAGS, O_POOL,
  P_COUNT
};

constexpr uint32_t kButtons = P_W_MIXER - P_REC + 1;

// Коды действий в порту «Out: Action». Те же числа в виджете.
enum Action : int {
  A_NONE, A_REC, A_UNDO, A_REDO, A_RESET_TRACK, A_RESET_ALL, A_MUTE, A_REVERSE, A_LOCK,
  A_TRK_NEXT, A_TRK_PREV, A_PART_NEXT, A_PART_PREV, A_PLAY, A_W_LOOP, A_W_MIXER
};

// Какое действие даёт тап и какое удержание. hold_ms = 0: срабатывает сразу по нажатию.
// С удержанием тап срабатывает при отпускании, иначе их не различить.
struct ButtonDef {
  Action tap, hold;
  double hold_ms;
};
const ButtonDef kDefs[kButtons] = {
    {A_REC, A_NONE, 0},
    {A_UNDO, A_REDO, 500},
    {A_REDO, A_NONE, 0},
    {A_RESET_TRACK, A_RESET_ALL, 1000},
    {A_MUTE, A_NONE, 0},
    {A_REVERSE, A_NONE, 0},
    {A_LOCK, A_NONE, 0},
    {A_TRK_NEXT, A_TRK_PREV, 500},
    {A_TRK_PREV, A_NONE, 0},
    {A_PART_NEXT, A_PART_PREV, 500},
    {A_PART_PREV, A_NONE, 0},
    {A_PLAY, A_NONE, 0},
    {A_W_LOOP, A_NONE, 0},
    {A_W_MIXER, A_NONE, 0},
};

// Настройки: LV2-параметры. Порядок и значения как в audio_looper.ttl.
enum Param { PR_REC_ORDER, PR_MODE, PR_LEN1, PR_LEN2, PR_LEN3, PR_LEN4, PR_FADE_OUT, PR_FADE_IN, PR_REV, PR_MUTE_Q, PR_COUNT };
struct ParamDef {
  const char* sym;
  int def, lo, hi;
};
const ParamDef kParams[PR_COUNT] = {
    {"rec_order", 0, 0, 1}, {"mode", 1, 0, 1},       {"len1", 0, 0, 16},      {"len2", 0, 0, 16},
    {"len3", 0, 0, 16},     {"len4", 0, 0, 16},      {"fade_out", 200, 10, 4000}, {"fade_in", 200, 10, 4000},
    {"rev_time", 50, 5, 1000}, {"mute_q", 2, 0, 3},
};
constexpr uint32_t kReportPerRun = 4;  // ответов на patch:Get за один run()

enum TempoSrc { SRC_NONE = 0, SRC_POSITION = 1, SRC_CLOCK = 2 };

struct Uris {
  LV2_URID atom_Object, atom_Blank, atom_Float, atom_Double, atom_Int, atom_Long, atom_URID, atom_Sequence;
  LV2_URID midi_Event;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_bpb, time_speed, time_frame;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID param[PR_COUNT];
};

struct Plugin {
  void* ports[P_COUNT] = {};
  double sr = 48000.0;
  Uris u = {};
  LV2_Atom_Forge forge;
  LV2_Worker_Schedule* schedule = nullptr;
  Looper* looper = nullptr;
  Button buttons[kButtons];
  TempoClock clock;
  MidiClock midi_clock;
  uint64_t now = 0;  // сэмплов от старта плагина
  int part = 0, widget = 0;
  int last_action = A_NONE;
  uint32_t action_count = 0;
  int32_t vals[PR_COUNT];
  int report_next = -1;  // следующий параметр для ответа на patch:Get, -1 = нечего

  float in(uint32_t p) const { return ports[p] ? *static_cast<const float*>(ports[p]) : 0.0f; }
  void out(uint32_t p, float v) {
    if (ports[p]) *static_cast<float*>(ports[p]) = v;
  }
  bool clock_is_source() const { return clock.messages() == 0 && midi_clock.active(now); }

  void apply_settings() {
    Settings s;
    s.rop = vals[PR_REC_ORDER] == 1;
    s.quantize = vals[PR_MODE] == 1;
    for (int i = 0; i < kTracks; ++i) s.length[i] = vals[PR_LEN1 + i];
    s.fade_out_ms = (float)vals[PR_FADE_OUT];
    s.fade_in_ms = (float)vals[PR_FADE_IN];
    s.reverse_ms = (float)vals[PR_REV];
    s.mute_q = vals[PR_MUTE_Q];
    s.dry = 1.0f;
    looper->set_settings(s);
  }
};

double atom_number(const Plugin* pl, const LV2_Atom* a, double def) {
  if (!a) return def;
  if (a->type == pl->u.atom_Float) return reinterpret_cast<const LV2_Atom_Float*>(a)->body;
  if (a->type == pl->u.atom_Double) return reinterpret_cast<const LV2_Atom_Double*>(a)->body;
  if (a->type == pl->u.atom_Int) return reinterpret_cast<const LV2_Atom_Int*>(a)->body;
  if (a->type == pl->u.atom_Long) return (double)reinterpret_cast<const LV2_Atom_Long*>(a)->body;
  return def;
}

int find_param(const Plugin* pl, LV2_URID key) {
  for (int i = 0; i < PR_COUNT; ++i)
    if (pl->u.param[i] == key) return i;
  return -1;
}

void set_param(Plugin* pl, int i, double v) {
  int x = (int)std::lround(v);
  if (x < kParams[i].lo) x = kParams[i].lo;
  if (x > kParams[i].hi) x = kParams[i].hi;
  pl->vals[i] = x;
}

void handle_position(Plugin* pl, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = nullptr, *bpb = nullptr, *speed = nullptr, *bar = nullptr, *bb = nullptr, *fr = nullptr;
  lv2_atom_object_get(obj, pl->u.time_bpm, &bpm, pl->u.time_bpb, &bpb, pl->u.time_speed, &speed,
                      pl->u.time_bar, &bar, pl->u.time_barBeat, &bb, pl->u.time_frame, &fr, 0);
  PositionMsg m;
  if (fr) { m.has_frame = true; m.frame = (int64_t)atom_number(pl, fr, 0); }
  if (bpm) { m.has_bpm = true; m.bpm = atom_number(pl, bpm, -1); }
  if (bpb) { m.has_bpb = true; m.bpb = atom_number(pl, bpb, -1); }
  if (speed) { m.has_speed = true; m.speed = atom_number(pl, speed, 0); }
  if (bar && bb) {
    m.has_pos = true;
    m.bar = atom_number(pl, bar, -1);
    m.bar_beat = atom_number(pl, bb, -1);
  }
  pl->clock.on_position(m);
}

void handle_patch_set(Plugin* pl, const LV2_Atom_Object* obj) {
  const LV2_Atom *prop = nullptr, *val = nullptr;
  lv2_atom_object_get(obj, pl->u.patch_property, &prop, pl->u.patch_value, &val, 0);
  if (!prop || prop->type != pl->u.atom_URID || !val) return;
  int i = find_param(pl, reinterpret_cast<const LV2_Atom_URID*>(prop)->body);
  if (i < 0) return;
  set_param(pl, i, atom_number(pl, val, pl->vals[i]));
  pl->apply_settings();
}

// Запасной путь: MIDI-клок. Используется, только пока time:Position не приходил ни разу.
void handle_midi(Plugin* pl, const uint8_t* b, uint32_t size, uint64_t t) {
  if (size < 1) return;
  switch (b[0]) {
    case 0xF8: pl->midi_clock.on_tick(t); break;
    case 0xFA: pl->midi_clock.on_start(); break;
    case 0xFB: pl->midi_clock.on_continue(); break;
    case 0xFC: pl->midi_clock.on_stop(); break;
    default: return;
  }
  if (!pl->clock_is_source()) return;
  PositionMsg m;
  m.has_speed = true;
  m.speed = pl->midi_clock.running() ? 1.0 : 0.0;
  double bpm = pl->midi_clock.bpm();
  if (bpm > 0) { m.has_bpm = true; m.bpm = bpm; }
  if (b[0] == 0xFA) m.has_pos = true;  // старт: такт 0, доля 0
  pl->clock.apply_external(m);
}

int wrap4(int v) { return (v % 4 + 4) % 4; }

void do_action(Plugin* pl, Action a) {
  Looper* lp = pl->looper;
  switch (a) {
    case A_NONE: return;
    case A_REC: lp->command(Cmd::Rec); break;
    case A_UNDO: lp->command(Cmd::Undo); break;
    case A_REDO: lp->command(Cmd::Redo); break;
    case A_RESET_TRACK: lp->command(Cmd::ResetTrack); break;
    case A_RESET_ALL: lp->command(Cmd::ResetAll); break;
    case A_MUTE: lp->command(Cmd::Mute); break;
    case A_REVERSE: lp->command(Cmd::Reverse); break;
    case A_TRK_NEXT: lp->command(Cmd::TrackNext); break;
    case A_TRK_PREV: lp->command(Cmd::TrackPrev); break;
    // Парты появятся на этапе 2; пока все пустые, а из пустого парта переход сразу (ТЗ).
    case A_PART_NEXT: pl->part = wrap4(pl->part + 1); break;
    case A_PART_PREV: pl->part = wrap4(pl->part - 1); break;
    case A_W_LOOP: pl->widget = 0; break;
    case A_W_MIXER: pl->widget = 1; break;
    default: break;  // Lock — этап 2, Play/Stop — этап 6
  }
  pl->last_action = a;
  ++pl->action_count;
}

// После patch:Get: сообщаем хосту значения параметров (по несколько за run).
void report_params(Plugin* pl) {
  auto* seq = static_cast<LV2_Atom_Sequence*>(pl->ports[P_EVENTS_OUT]);
  if (!seq) return;
  const uint32_t cap = seq->atom.size;
  lv2_atom_forge_set_buffer(&pl->forge, reinterpret_cast<uint8_t*>(seq), cap);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_sequence_head(&pl->forge, &frame, 0);
  for (uint32_t k = 0; k < kReportPerRun && pl->report_next >= 0; ++k) {
    int i = pl->report_next;
    LV2_Atom_Forge_Frame of;
    lv2_atom_forge_frame_time(&pl->forge, 0);
    lv2_atom_forge_object(&pl->forge, &of, 0, pl->u.patch_Set);
    lv2_atom_forge_key(&pl->forge, pl->u.patch_property);
    lv2_atom_forge_urid(&pl->forge, pl->u.param[i]);
    lv2_atom_forge_key(&pl->forge, pl->u.patch_value);
    lv2_atom_forge_int(&pl->forge, pl->vals[i]);
    lv2_atom_forge_pop(&pl->forge, &of);
    pl->report_next = i + 1 < PR_COUNT ? i + 1 : -1;
  }
  lv2_atom_forge_pop(&pl->forge, &frame);
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features) {
  LV2_URID_Map* map = nullptr;
  LV2_Worker_Schedule* schedule = nullptr;
  for (int i = 0; features && features[i]; ++i) {
    if (!std::strcmp(features[i]->URI, LV2_URID__map)) map = static_cast<LV2_URID_Map*>(features[i]->data);
    if (!std::strcmp(features[i]->URI, LV2_WORKER__schedule))
      schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
  }
  if (!map || !schedule) return nullptr;

  Plugin* pl = new (std::nothrow) Plugin();
  if (!pl) return nullptr;
  pl->looper = new (std::nothrow) Looper();
  if (!pl->looper || !pl->looper->init(rate, kPoolBytes)) {
    delete pl->looper;
    delete pl;
    return nullptr;
  }
  pl->sr = rate;
  pl->schedule = schedule;
  Uris& u = pl->u;
  auto M = [map](const char* uri) { return map->map(map->handle, uri); };
  u.atom_Object = M(LV2_ATOM__Object);
  u.atom_Blank = M(LV2_ATOM__Blank);
  u.atom_Float = M(LV2_ATOM__Float);
  u.atom_Double = M(LV2_ATOM__Double);
  u.atom_Int = M(LV2_ATOM__Int);
  u.atom_Long = M(LV2_ATOM__Long);
  u.atom_URID = M(LV2_ATOM__URID);
  u.atom_Sequence = M(LV2_ATOM__Sequence);
  u.midi_Event = M(LV2_MIDI__MidiEvent);
  u.time_Position = M(LV2_TIME__Position);
  u.time_bar = M(LV2_TIME__bar);
  u.time_barBeat = M(LV2_TIME__barBeat);
  u.time_bpm = M(LV2_TIME__beatsPerMinute);
  u.time_bpb = M(LV2_TIME__beatsPerBar);
  u.time_speed = M(LV2_TIME__speed);
  u.time_frame = M(LV2_TIME__frame);
  u.patch_Set = M(LV2_PATCH__Set);
  u.patch_Get = M(LV2_PATCH__Get);
  u.patch_property = M(LV2_PATCH__property);
  u.patch_value = M(LV2_PATCH__value);
  for (int i = 0; i < PR_COUNT; ++i) {
    char uri[96];
    std::snprintf(uri, sizeof uri, "%s#%s", AL_URI, kParams[i].sym);
    u.param[i] = M(uri);
    pl->vals[i] = kParams[i].def;
  }
  lv2_atom_forge_init(&pl->forge, map);
  pl->apply_settings();

  pl->clock.set_sample_rate(rate);
  pl->midi_clock.set_sample_rate(rate);
  for (uint32_t i = 0; i < kButtons; ++i) {
    const ButtonDef& d = kDefs[i];
    pl->buttons[i].configure(d.hold_ms > 0, (uint32_t)(d.hold_ms * rate / 1000.0));
  }
  return pl;
}

void connect_port(LV2_Handle h, uint32_t port, void* data) {
  if (port < P_COUNT) static_cast<Plugin*>(h)->ports[port] = data;
}

void run(LV2_Handle h, uint32_t n) {
  Plugin* pl = static_cast<Plugin*>(h);
  Looper* lp = pl->looper;

  // Время блока — по часам на его начало (сообщения о позиции действуют со следующего блока).
  TimeInfo ti;
  ti.beats = pl->clock.beats();
  ti.beats_per_frame = pl->clock.bpm() / 60.0 / pl->sr;
  ti.bpb = pl->clock.bpb();
  ti.rolling = pl->clock.rolling();

  // События: темп, MIDI-клок, настройки. Часы двигаем кусками между событиями.
  uint32_t done = 0;
  if (const auto* seq = static_cast<const LV2_Atom_Sequence*>(pl->ports[P_EVENTS])) {
    LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
      uint32_t f = ev->time.frames < n ? (uint32_t)ev->time.frames : n;
      if (f > done) { pl->clock.advance(f - done); done = f; }
      if (ev->body.type == pl->u.midi_Event) {
        handle_midi(pl, reinterpret_cast<const uint8_t*>(&ev->body + 1), ev->body.size, pl->now + f);
      } else if (ev->body.type == pl->u.atom_Object || ev->body.type == pl->u.atom_Blank) {
        const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
        if (obj->body.otype == pl->u.time_Position) handle_position(pl, obj);
        else if (obj->body.otype == pl->u.patch_Set) handle_patch_set(pl, obj);
        else if (obj->body.otype == pl->u.patch_Get) pl->report_next = 0;
      }
    }
  }
  if (n > done) pl->clock.advance(n - done);
  pl->now += n;

  // Контроллеры-действия -> команды ядру (до звука, чтобы нажатие попало в этот блок).
  lp->set_time(ti);
  uint32_t held = 0;
  for (uint32_t i = 0; i < kButtons; ++i) {
    Gesture g = pl->buttons[i].process(pl->in(P_REC + i), n);
    if (g == Gesture::Tap) do_action(pl, kDefs[i].tap);
    if (g == Gesture::Hold) do_action(pl, kDefs[i].hold);
    if (pl->buttons[i].down()) held |= 1u << i;
  }

  lp->process(static_cast<const float*>(pl->ports[P_IN_L]), static_cast<const float*>(pl->ports[P_IN_R]),
              static_cast<float*>(pl->ports[P_OUT_L]), static_cast<float*>(pl->ports[P_OUT_R]), n, ti);

  WorkMsg m;
  if (lp->take_work(m)) {
    if (pl->schedule->schedule_work(pl->schedule->handle, sizeof m, &m) != LV2_WORKER_SUCCESS) lp->cancel_work();
  }

  report_params(pl);

  // Выходы. Такт и доля — по сетке лупера (при стоящем транспорте он считает такты сам).
  const double gb = lp->grid_beats();
  const int bpb = pl->clock.bpb();
  const double bar = std::floor(gb / bpb);
  int src = pl->clock.messages() > 0 ? SRC_POSITION : (pl->midi_clock.active(pl->now) ? SRC_CLOCK : SRC_NONE);
  pl->out(O_TRACK, (float)lp->active());
  pl->out(O_PART, (float)pl->part);
  pl->out(O_PEND, -1.0f);
  pl->out(O_WIDGET, (float)pl->widget);
  pl->out(O_ACTION, (float)pl->last_action);
  pl->out(O_ACT_CNT, (float)(pl->action_count % 1000000));
  pl->out(O_HELD, (float)held);
  pl->out(O_BPM, (float)pl->clock.bpm());
  pl->out(O_BAR, (float)std::fmod(bar, 1000000.0));
  pl->out(O_BEAT, (float)(gb - bar * bpb));
  pl->out(O_BPB, (float)bpb);
  pl->out(O_ROLLING, pl->clock.rolling() ? 1.0f : 0.0f);
  pl->out(O_TSRC, (float)src);
  pl->out(O_CLK_BPM, (float)pl->midi_clock.bpm());
  pl->out(O_POS_CNT, (float)(pl->clock.messages() % 1000000));
  uint32_t flags = 0;
  for (int i = 0; i < kTracks; ++i) {
    TrackView v = lp->view(i);
    pl->out(O_ST1 + i, (float)v.state);
    pl->out(O_POS1 + i, v.pos);
    pl->out(O_LEN1 + i, v.len_s);
    if (v.muted) flags |= 1u << i;
    if (v.reversed) flags |= 1u << (4 + i);
    if (v.can_undo) flags |= 1u << (8 + i);
    if (v.can_redo) flags |= 1u << (12 + i);
    if (v.stopped) flags |= 1u << (16 + i);
  }
  pl->out(O_FLAGS, (float)flags);
  pl->out(O_POOL, lp->pool_fill());
}

void cleanup(LV2_Handle h) {
  Plugin* pl = static_cast<Plugin*>(h);
  delete pl->looper;
  delete pl;
}

// ---- рабочий поток ----
LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                       uint32_t size, const void* data) {
  if (size != sizeof(WorkMsg)) return LV2_WORKER_ERR_UNKNOWN;
  WorkMsg m;
  std::memcpy(&m, data, sizeof m);
  WorkResp r;
  static_cast<Plugin*>(h)->looper->do_work(m, r);
  return respond(rh, sizeof r, &r);
}

LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
  if (size != sizeof(WorkResp)) return LV2_WORKER_ERR_UNKNOWN;
  WorkResp r;
  std::memcpy(&r, data, sizeof r);
  static_cast<Plugin*>(h)->looper->work_done(r);
  return LV2_WORKER_SUCCESS;
}

// ---- state: настройки попадают в снапшоты и пресеты Zynthian ----
LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t,
                      const LV2_Feature* const*) {
  Plugin* pl = static_cast<Plugin*>(h);
  for (int i = 0; i < PR_COUNT; ++i)
    store(sh, pl->u.param[i], &pl->vals[i], sizeof(int32_t), pl->u.atom_Int,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  return LV2_STATE_SUCCESS;
}

LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh, uint32_t,
                         const LV2_Feature* const*) {
  Plugin* pl = static_cast<Plugin*>(h);
  for (int i = 0; i < PR_COUNT; ++i) {
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* v = retrieve(sh, pl->u.param[i], &size, &type, &flags);
    if (v && type == pl->u.atom_Int && size == sizeof(int32_t)) set_param(pl, i, *static_cast<const int32_t*>(v));
  }
  pl->apply_settings();
  pl->report_next = 0;  // сообщить хосту и виджету новые значения
  return LV2_STATE_SUCCESS;
}

const void* extension_data(const char* uri) {
  static const LV2_Worker_Interface worker = {work, work_response, nullptr};
  static const LV2_State_Interface state = {save, restore};
  if (!std::strcmp(uri, LV2_WORKER__interface)) return &worker;
  if (!std::strcmp(uri, LV2_STATE__interface)) return &state;
  return nullptr;
}

const LV2_Descriptor kDescriptor = {AL_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, extension_data};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}
