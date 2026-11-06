// Audio Looper: тонкая LV2-обёртка над ядром из core/.
// Этап 0: звук проходит насквозь, контроллеры-действия распознаются (тап и удержание),
// темп и позиция берутся из time:Position, запасной путь — MIDI-клок.
#include <cstdint>
#include <cstring>
#include <new>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include "../core/gestures.h"
#include "../core/tempo.h"

#define AL_URI "urn:audiolooper:looper"

namespace {

using namespace alp;

// Порядок совпадает с lv2:index в audio_looper.ttl.
enum Port : uint32_t {
  P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_EVENTS,
  // страницы 1–4: контроллеры-действия
  P_REC, P_UNDO, P_REDO, P_RESET,
  P_MUTE, P_REVERSE, P_LOCK, P_TRK_NEXT,
  P_TRK_PREV, P_PART_NEXT, P_PART_PREV, P_PLAY,
  P_W_LOOP, P_W_MIXER,
  // страница 5: плавные (на этапе 0 звук не меняют)
  P_LEVEL, P_PAN, P_MASTER, P_SPEED,
  // выходы только для чтения
  O_TRACK, O_PART, O_PEND, O_WIDGET, O_ACTION, O_ACT_CNT, O_HELD,
  O_BPM, O_BAR, O_BEAT, O_BPB, O_ROLLING, O_TSRC, O_CLK_BPM, O_POS_CNT,
  P_COUNT
};

constexpr uint32_t kButtons = P_W_MIXER - P_REC + 1;

// Коды действий в порту «Out: Action». Те же числа в виджете.
enum Action : int {
  A_NONE, A_REC, A_UNDO, A_REDO, A_RESET_TRACK, A_RESET_ALL, A_MUTE, A_REVERSE, A_LOCK,
  A_TRK_NEXT, A_TRK_PREV, A_PART_NEXT, A_PART_PREV, A_PLAY, A_W_LOOP, A_W_MIXER
};

// Какое действие даёт тап и какое удержание для каждого контроллера.
// hold_ms = 0: кнопка без удержания, срабатывает сразу по нажатию.
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

enum TempoSrc { SRC_NONE = 0, SRC_POSITION = 1, SRC_CLOCK = 2 };

struct Uris {
  LV2_URID atom_Object, atom_Blank, atom_Float, atom_Double, atom_Int, atom_Long;
  LV2_URID midi_Event;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_bpb, time_speed;
};

struct Plugin {
  void* ports[P_COUNT] = {};
  double sr = 48000.0;
  Uris u = {};
  Button buttons[kButtons];
  TempoClock clock;
  MidiClock midi_clock;
  uint64_t now = 0;  // сэмплов от старта плагина
  int track = 0, part = 0, widget = 0;
  int last_action = A_NONE;
  uint32_t action_count = 0;

  float in(uint32_t p) const { return ports[p] ? *static_cast<const float*>(ports[p]) : 0.0f; }
  void out(uint32_t p, float v) {
    if (ports[p]) *static_cast<float*>(ports[p]) = v;
  }
  bool clock_is_source() const { return clock.messages() == 0 && midi_clock.active(now); }
};

double atom_number(const Plugin* pl, const LV2_Atom* a) {
  if (a->type == pl->u.atom_Float) return reinterpret_cast<const LV2_Atom_Float*>(a)->body;
  if (a->type == pl->u.atom_Double) return reinterpret_cast<const LV2_Atom_Double*>(a)->body;
  if (a->type == pl->u.atom_Int) return reinterpret_cast<const LV2_Atom_Int*>(a)->body;
  if (a->type == pl->u.atom_Long) return (double)reinterpret_cast<const LV2_Atom_Long*>(a)->body;
  return -1.0;
}

void handle_position(Plugin* pl, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = nullptr, *bpb = nullptr, *speed = nullptr, *bar = nullptr, *bb = nullptr;
  lv2_atom_object_get(obj, pl->u.time_bpm, &bpm, pl->u.time_bpb, &bpb, pl->u.time_speed, &speed,
                      pl->u.time_bar, &bar, pl->u.time_barBeat, &bb, 0);
  PositionMsg m;
  if (bpm) { m.has_bpm = true; m.bpm = atom_number(pl, bpm); }
  if (bpb) { m.has_bpb = true; m.bpb = atom_number(pl, bpb); }
  if (speed) { m.has_speed = true; m.speed = atom_number(pl, speed); }
  if (bar && bb) {
    m.has_pos = true;
    m.bar = atom_number(pl, bar);
    m.bar_beat = atom_number(pl, bb);
  }
  pl->clock.on_position(m);
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
  if (b[0] == 0xFA) { m.has_pos = true; }  // старт: такт 0, доля 0
  pl->clock.apply_external(m);
}

int wrap4(int v) { return (v % 4 + 4) % 4; }

void do_action(Plugin* pl, Action a) {
  switch (a) {
    case A_NONE: return;
    case A_TRK_NEXT: pl->track = wrap4(pl->track + 1); break;
    case A_TRK_PREV: pl->track = wrap4(pl->track - 1); break;
    // Все парты пока пустые, а из пустого парта переход сразу (ТЗ).
    case A_PART_NEXT: pl->part = wrap4(pl->part + 1); break;
    case A_PART_PREV: pl->part = wrap4(pl->part - 1); break;
    case A_W_LOOP: pl->widget = 0; break;
    case A_W_MIXER: pl->widget = 1; break;
    default: break;  // остальное появится на этапе 1, пока видно в «Out: Action»
  }
  pl->last_action = a;
  ++pl->action_count;
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features) {
  LV2_URID_Map* map = nullptr;
  for (int i = 0; features && features[i]; ++i)
    if (!std::strcmp(features[i]->URI, LV2_URID__map)) map = static_cast<LV2_URID_Map*>(features[i]->data);
  if (!map) return nullptr;

  Plugin* pl = new (std::nothrow) Plugin();
  if (!pl) return nullptr;
  pl->sr = rate;
  Uris& u = pl->u;
  u.atom_Object = map->map(map->handle, LV2_ATOM__Object);
  u.atom_Blank = map->map(map->handle, LV2_ATOM__Blank);
  u.atom_Float = map->map(map->handle, LV2_ATOM__Float);
  u.atom_Double = map->map(map->handle, LV2_ATOM__Double);
  u.atom_Int = map->map(map->handle, LV2_ATOM__Int);
  u.atom_Long = map->map(map->handle, LV2_ATOM__Long);
  u.midi_Event = map->map(map->handle, LV2_MIDI__MidiEvent);
  u.time_Position = map->map(map->handle, LV2_TIME__Position);
  u.time_bar = map->map(map->handle, LV2_TIME__bar);
  u.time_barBeat = map->map(map->handle, LV2_TIME__barBeat);
  u.time_bpm = map->map(map->handle, LV2_TIME__beatsPerMinute);
  u.time_bpb = map->map(map->handle, LV2_TIME__beatsPerBar);
  u.time_speed = map->map(map->handle, LV2_TIME__speed);

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

  // События: темп и MIDI-клок. Часы двигаем кусками между событиями.
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
      }
    }
  }
  if (n > done) pl->clock.advance(n - done);
  pl->now += n;

  // Звук: пока насквозь.
  for (int c = 0; c < 2; ++c) {
    const float* src = static_cast<const float*>(pl->ports[P_IN_L + c]);
    float* dst = static_cast<float*>(pl->ports[P_OUT_L + c]);
    if (!dst) continue;
    if (!src) std::memset(dst, 0, n * sizeof(float));
    else if (src != dst) std::memcpy(dst, src, n * sizeof(float));
  }

  // Контроллеры-действия.
  uint32_t held = 0;
  for (uint32_t i = 0; i < kButtons; ++i) {
    Gesture g = pl->buttons[i].process(pl->in(P_REC + i), n);
    if (g == Gesture::Tap) do_action(pl, kDefs[i].tap);
    if (g == Gesture::Hold) do_action(pl, kDefs[i].hold);
    if (pl->buttons[i].down()) held |= 1u << i;
  }

  int src = pl->clock.messages() > 0 ? SRC_POSITION : (pl->midi_clock.active(pl->now) ? SRC_CLOCK : SRC_NONE);
  pl->out(O_TRACK, (float)pl->track);
  pl->out(O_PART, (float)pl->part);
  pl->out(O_PEND, -1.0f);
  pl->out(O_WIDGET, (float)pl->widget);
  pl->out(O_ACTION, (float)pl->last_action);
  pl->out(O_ACT_CNT, (float)(pl->action_count % 1000000));
  pl->out(O_HELD, (float)held);
  pl->out(O_BPM, (float)pl->clock.bpm());
  pl->out(O_BAR, (float)(pl->clock.bar() % 1000000));
  pl->out(O_BEAT, (float)pl->clock.bar_beat());
  pl->out(O_BPB, (float)pl->clock.bpb());
  pl->out(O_ROLLING, pl->clock.rolling() ? 1.0f : 0.0f);
  pl->out(O_TSRC, (float)src);
  pl->out(O_CLK_BPM, (float)pl->midi_clock.bpm());
  pl->out(O_POS_CNT, (float)(pl->clock.messages() % 1000000));
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor kDescriptor = {AL_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, nullptr};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}
