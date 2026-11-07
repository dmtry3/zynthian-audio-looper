// Офлайн-тесты ядра. Запуск: make test
#include <cmath>
#include <cstdio>

#include "../core/gestures.h"
#include "../core/tempo.h"

using namespace alp;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (cond) {                                                    \
      ++g_pass;                                                    \
    } else {                                                       \
      ++g_fail;                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                              \
  } while (0)

static const double SR = 48000.0;
static const uint32_t BLOCK = 128;
static uint32_t ms(double v) { return (uint32_t)(v * SR / 1000.0); }

// Держит кнопку value в течение samples, возвращает число тапов и удержаний.
struct Count {
  int tap = 0, hold = 0;
};
static void run(Button& b, float value, uint32_t samples, Count& c) {
  for (uint32_t t = 0; t < samples; t += BLOCK) {
    Gesture g = b.process(value, BLOCK);
    if (g == Gesture::Tap) ++c.tap;
    if (g == Gesture::Hold) ++c.hold;
  }
}

static void test_gestures() {
  {  // без удержания: тап сразу по нажатию, отпускание ничего не даёт
    Button b;
    b.configure(false, 0);
    Count c;
    run(b, 0, BLOCK, c);
    CHECK(b.process(1, BLOCK) == Gesture::Tap);
    run(b, 1, ms(2000), c);
    run(b, 0, ms(100), c);
    CHECK(c.tap == 0 && c.hold == 0);
  }
  {  // с удержанием: короткое нажатие = тап при отпускании
    Button b;
    b.configure(true, ms(500));
    Count c;
    run(b, 0, BLOCK, c);
    run(b, 1, ms(200), c);
    CHECK(c.tap == 0);
    run(b, 0, ms(50), c);
    CHECK(c.tap == 1 && c.hold == 0);
  }
  {  // с удержанием: удержание срабатывает на пороге, не дожидаясь отпускания
    Button b;
    b.configure(true, ms(500));
    Count c;
    run(b, 0, BLOCK, c);
    run(b, 1, ms(490), c);
    CHECK(c.hold == 0);
    run(b, 1, ms(20), c);
    CHECK(c.hold == 1);
    run(b, 1, ms(2000), c);
    run(b, 0, ms(50), c);
    CHECK(c.tap == 0 && c.hold == 1);
  }
  {  // Reset: порог 1 с, 700 мс ещё тап
    Button b;
    b.configure(true, ms(1000));
    Count c;
    run(b, 0, BLOCK, c);
    run(b, 1, ms(700), c);
    run(b, 0, ms(50), c);
    CHECK(c.tap == 1 && c.hold == 0);
    run(b, 1, ms(1100), c);
    CHECK(c.hold == 1);
  }
  {  // значение 1 в первом блоке (из снапшота) не нажатие
    Button b;
    b.configure(false, 0);
    Count c;
    run(b, 1, ms(100), c);
    run(b, 0, ms(100), c);
    CHECK(c.tap == 0);
    run(b, 1, BLOCK, c);
    CHECK(c.tap == 1);
  }
}

static void test_tempo_clock() {
  TempoClock k;
  k.set_sample_rate(SR);
  PositionMsg m;
  m.has_bpm = m.has_bpb = m.has_speed = m.has_pos = true;
  m.bpm = 120;
  m.bpb = 4;
  m.speed = 1;
  m.bar = 2;
  m.bar_beat = 3.5;
  k.on_position(m);
  CHECK(k.rolling() && k.bpm() == 120 && k.bpb() == 4);
  CHECK(k.bar() == 2 && std::fabs(k.bar_beat() - 3.5) < 1e-9);
  // 120 уд/мин: полдоли = 0,25 с = 12000 сэмплов, переходим на такт 3, долю 0
  for (int i = 0; i < 12000 / 125; ++i) k.advance(125);
  CHECK(k.bar() == 3 && std::fabs(k.bar_beat()) < 1e-9);
  // сообщение только с темпом не трогает позицию
  PositionMsg t;
  t.has_bpm = true;
  t.bpm = 140;
  k.on_position(t);
  CHECK(k.bpm() == 140 && k.bar() == 3);
  // нелепый темп игнорируется
  t.bpm = 0;
  k.on_position(t);
  CHECK(k.bpm() == 140);
  CHECK(k.messages() == 3);
}

// Остановка и повторный пуск: пока стоит — позиция стоит; пуск без такта и доли — по кадру.
static void test_tempo_stop_start() {
  TempoClock k;
  k.set_sample_rate(SR);
  PositionMsg m;
  m.has_bpm = m.has_bpb = m.has_speed = m.has_pos = m.has_frame = true;
  m.bpm = 120; m.bpb = 4; m.speed = 1; m.bar = 0; m.bar_beat = 0; m.frame = 0;
  k.on_position(m);
  for (int i = 0; i < 375; ++i) k.advance(128);  // 1 с = 2 доли
  CHECK(std::fabs(k.beats() - 2.0) < 1e-9);
  PositionMsg stop;
  stop.has_speed = stop.has_frame = true;
  stop.speed = 0; stop.frame = 48000;
  k.on_position(stop);
  for (int i = 0; i < 375 * 3; ++i) k.advance(128);  // стоим 3 с
  CHECK(std::fabs(k.beats() - 2.0) < 1e-9);         // позиция стоит
  PositionMsg go;                                    // пуск с того же места, без такта и доли
  go.has_speed = go.has_frame = true;
  go.speed = 1; go.frame = 48000;
  k.on_position(go);
  CHECK(k.rolling() && std::fabs(k.beats() - 2.0) < 1e-9);
  for (int i = 0; i < 375; ++i) k.advance(128);
  CHECK(std::fabs(k.beats() - 4.0) < 1e-9);
  PositionMsg zero;                                  // перемотка в начало без такта и доли
  zero.has_speed = zero.has_frame = true;
  zero.speed = 1; zero.frame = 0;
  k.on_position(zero);
  CHECK(std::fabs(k.beats()) < 1e-9);
}

// Долей в такте из настройки: пока Zynthian не прислал размер; счёт такта не прыгает.
static void test_default_bpb() {
  TempoClock k;
  k.set_sample_rate(SR);
  CHECK(k.bpb() == 4 && !k.bpb_from_host());
  PositionMsg m;
  m.has_bpm = m.has_speed = m.has_pos = true;  // без размера такта
  m.bpm = 120; m.speed = 1; m.bar = 2; m.bar_beat = 1.5;
  k.on_position(m);
  k.set_default_bpb(3);
  CHECK(k.bpb() == 3 && k.bar() == 2 && std::fabs(k.bar_beat() - 1.5) < 1e-9);
  k.set_default_bpb(0);  // вне диапазона — без изменений
  CHECK(k.bpb() == 3);
  PositionMsg z;
  z.has_bpb = true;
  z.bpb = 7;
  k.on_position(z);
  CHECK(k.bpb() == 7 && k.bpb_from_host());
  k.set_default_bpb(5);  // Zynthian главнее
  CHECK(k.bpb() == 7);
}

static void test_midi_clock() {
  MidiClock c;
  c.set_sample_rate(SR);
  CHECK(c.bpm() == 0.0);
  // 130 уд/мин: тик каждые 60/130/24 с
  const double step = SR * 60.0 / 130.0 / 24.0;
  c.on_start();
  uint64_t t = 0;
  for (int i = 0; i < 48; ++i) {
    t = (uint64_t)std::llround(i * step);
    c.on_tick(t);
  }
  CHECK(std::fabs(c.bpm() - 130.0) < 0.1);
  CHECK(c.active(t + 100) && !c.active(t + (uint64_t)SR));
  CHECK(std::fabs(c.beats() - 2.0) < 1e-9);
  c.on_stop();
  CHECK(!c.running());
}

int main() {
  test_gestures();
  test_tempo_clock();
  test_tempo_stop_start();
  test_default_bpb();
  test_midi_clock();
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
