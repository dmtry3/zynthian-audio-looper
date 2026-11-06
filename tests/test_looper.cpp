// Офлайн-тесты ядра лупера. Запуск: make test. Результат для прослушивания: tests/out/*.wav
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "../core/looper.h"

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
static const double PI2 = 6.283185307179586;

// Имитация хоста: блоки по 128, рабочий поток выполняется сразу после блока.
struct Sim {
  std::unique_ptr<Looper> lp{new Looper()};
  Settings set;
  TimeInfo ti;
  double bpm = 120.0;
  uint64_t t = 0;  // кадров от старта
  std::function<float(uint64_t)> in_l = [](uint64_t) { return 0.0f; };
  std::function<float(uint64_t)> in_r;
  std::vector<float> out_l, out_r;
  std::function<double(uint64_t)> beats_at;  // позиция транспорта в долях (по умолчанию от t = 0)
  double beats_now() const { return beats_at ? beats_at(t) : (double)t * bpm / 60.0 / SR; }

  std::unique_ptr<PeakShm> peaks{new PeakShm()};
  explicit Sim(size_t pool_mb = 64, bool quantize = false) {
    lp->init(SR, pool_mb << 20);
    lp->set_peaks(peaks.get());
    set.quantize = quantize;
    set.dry = 0.0f;
    lp->set_settings(set);
  }
  void apply() { lp->set_settings(set); }
  void run(uint64_t frames) {
    float il[BLOCK], ir[BLOCK], ol[BLOCK], orr[BLOCK];
    for (uint64_t done = 0; done < frames; done += BLOCK) {
      for (uint32_t i = 0; i < BLOCK; ++i) {
        il[i] = in_l(t + i);
        ir[i] = in_r ? in_r(t + i) : il[i] * 0.5f;
      }
      ti.beats_per_frame = bpm / 60.0 / SR;
      ti.beats = beats_now();
      lp->process(il, ir, ol, orr, BLOCK, ti);
      WorkMsg m;
      while (lp->take_work(m)) {
        WorkResp r;
        lp->do_work(m, r);
        lp->work_done(r);
      }
      out_l.insert(out_l.end(), ol, ol + BLOCK);
      out_r.insert(out_r.end(), orr, orr + BLOCK);
      t += BLOCK;
    }
  }
  void cmd(Cmd c) {
    ti.beats_per_frame = bpm / 60.0 / SR;
    ti.beats = beats_now();
    lp->set_time(ti);
    lp->command(c);
  }
  TrackView v(int i = 0) const { return lp->view(i); }
  // самый большой скачок между соседними сэмплами на отрезке
  float max_step(uint64_t from, uint64_t to) const {
    float m = 0;
    for (uint64_t i = from + 1; i < to && i < out_l.size(); ++i) {
      m = std::fmax(m, std::fabs(out_l[i] - out_l[i - 1]));
      m = std::fmax(m, std::fabs(out_r[i] - out_r[i - 1]));
    }
    return m;
  }
  std::vector<float> slice(uint64_t from, uint64_t n) const {
    return std::vector<float>(out_l.begin() + from, out_l.begin() + from + n);
  }
  void wav(const char* name) const {
    char path[256];
    std::snprintf(path, sizeof path, "tests/out/%s.wav", name);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t n = (uint32_t)out_l.size(), data = n * 8, sr = (uint32_t)SR, br = sr * 8, sz = 36 + data;
    uint16_t fmt = 3, ch = 2, ba = 8, bits = 32;
    uint32_t sixteen = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&sz, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&sixteen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&out_l[i], 4, 1, f); std::fwrite(&out_r[i], 4, 1, f); }
    std::fclose(f);
  }
};

static float rms_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double s = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
  return (float)std::sqrt(s / (double)a.size());
}
static float rms(const std::vector<float>& a) { return rms_diff(a, std::vector<float>(a.size(), 0.0f)); }
static auto sine(double f, float a) {
  return [f, a](uint64_t t) { return a * (float)std::sin(PI2 * f * (double)t / SR); };
}

// Free, RPO: записали секунду, луп играет записанное, стык и старт без щелчков.
static void test_record_play() {
  Sim s;
  s.in_l = sine(230.3, 0.5f);  // не целое число периодов на цикл: стык должен сгладить кроссфейд
  s.cmd(Cmd::Rec);
  s.run(48000);
  CHECK(s.v().state == 2);
  s.cmd(Cmd::Rec);
  uint64_t t_close = s.t;
  s.run(BLOCK * 4);  // хвост 10 мс ещё звучит на входе
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(48000 * 3);
  s.wav("record_play");
  CHECK(s.v().state == 3);
  CHECK(std::fabs(s.v().len_s - 1.0f) < 1e-3f);
  CHECK(s.max_step(0, s.t) < 0.03f);
  // второй цикл: то, что записали (кроме стыка)
  uint64_t L = 48000;
  float err = 0;
  for (uint64_t p = 1000; p < L - 10; ++p) {
    float got = s.out_l[t_close + L + p];
    float want = 0.5f * (float)std::sin(PI2 * 230.3 * (double)p / SR);
    err = std::fmax(err, std::fabs(got - want));
  }
  CHECK(err < 1e-4f);
}

// 10 овердабов подряд, undo по одному слою возвращает звук, redo возвращает слой.
static void test_overdubs_undo() {
  Sim s;
  const uint64_t L = 375 * BLOCK;  // 48000
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  const uint64_t t0 = s.t;  // позиция 0 цикла совпадает с t0 + k*L
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(L);  // хвост уже тишина: проверяем чистую основу
  std::vector<std::vector<float>> ref;
  auto cycle_start = [&]() { return t0 + ((s.t - t0 + L - 1) / L) * L; };
  auto capture = [&]() {
    uint64_t c = cycle_start();
    s.run(c - s.t + L);
    return s.slice(c, L);
  };
  ref.push_back(capture());
  for (int k = 1; k <= 10; ++k) {
    // овердаб ровно на цикл: нажали на начале цикла, отжали через цикл
    s.run(cycle_start() - s.t);
    double f = 300.0 + 37.0 * k;
    s.in_l = sine(f, 0.05f);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 4);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);  // затухание конца овердаба
    s.in_l = [](uint64_t) { return 0.0f; };
    CHECK(s.v().state == 3 && s.v().layers == (uint32_t)(k + 1));
    ref.push_back(capture());
  }
  s.wav("overdubs");
  CHECK(s.max_step(t0, s.t) < 0.08f);
  // каждый слой что-то добавил
  for (int k = 1; k <= 10; ++k) CHECK(rms_diff(ref[k], ref[k - 1]) > 0.01f);
  uint64_t t_undo = s.t;
  for (int k = 10; k >= 1; --k) {
    s.cmd(Cmd::Undo);
    s.run(BLOCK * 8);
    std::vector<float> got = capture();
    CHECK(rms_diff(got, ref[k - 1]) < 1e-4f);
  }
  CHECK(s.max_step(t_undo, s.t) < 0.08f);
  s.cmd(Cmd::Redo);
  s.run(BLOCK * 8);
  CHECK(rms_diff(capture(), ref[1]) < 1e-4f);
  CHECK(s.v().can_undo && s.v().can_redo);
  // undo основы: дорожка пустеет; ещё undo на пустой — молча ничего
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 0);
  s.cmd(Cmd::Undo);
  s.run(L / 2);
  CHECK(s.v().state == 0);
  CHECK(rms(s.slice(s.t - L / 4, L / 4)) < 1e-6f);
}

// Mute: плавно по fade out, без щелчков, unmute возвращает.
static void test_mute() {
  Sim s;
  s.in_l = sine(220.0, 0.5f);
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);
  s.run(48000);
  uint64_t tm = s.t;
  s.cmd(Cmd::Mute);  // Free: сразу
  s.run(48000 / 2);
  CHECK(s.v().muted);
  CHECK(rms(s.slice(s.t - 2000, 2000)) < 1e-6f);  // через 200 мс тишина
  CHECK(rms(s.slice(tm, 2000)) > 0.1f);            // но не сразу
  s.cmd(Cmd::Mute);
  s.run(48000);
  CHECK(rms(s.slice(s.t - 4800, 4800)) > 0.2f);
  CHECK(s.max_step(tm - 100, s.t) < 0.03f);
  s.wav("mute");
}

// Reverse как лента: разворот без щелчков, потом звук идёт задом наперёд.
static void test_reverse() {
  Sim s;
  const uint64_t L = 48000;
  s.in_l = sine(180.0, 0.5f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 4);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(L * 2);
  uint64_t c = s.t - (s.t % 1);  // эталон: цикл вперёд, начиная с позиции 0
  (void)c;
  std::vector<float> fwd(L);
  {
    // позиция p звучит в момент t_close + k*L + p; t_close = L (запись началась в 0)
    uint64_t base = L + L;  // второй цикл
    for (uint64_t p = 0; p < L; ++p) fwd[p] = s.out_l[base + p];
  }
  uint64_t tr = s.t;
  s.cmd(Cmd::Reverse);
  s.run(BLOCK * 40);  // 50 мс разворот
  CHECK(s.v().reversed);
  CHECK(s.max_step(tr - 10, s.t) < 0.03f);
  // после разворота: каждый следующий сэмпл — предыдущая позиция
  uint64_t p = (uint64_t)std::llround(s.v().pos * (double)L) % L;
  uint64_t t1 = s.t;
  s.run(4096);
  float err = 0;
  for (uint64_t k = 0; k < 2000; ++k) {
    uint64_t pk = (p + L - k) % L;
    if (pk < 600) break;  // область стыка не сравниваем
    err = std::fmax(err, std::fabs(s.out_l[t1 + k] - fwd[pk]));
  }
  CHECK(err < 1e-4f);
  s.cmd(Cmd::Reverse);
  s.run(BLOCK * 40);
  CHECK(!s.v().reversed && s.max_step(tr, s.t) < 0.03f);
  s.wav("reverse");
}

// Quantize: запись ждёт такта, фиксированная длина закрывается сама; Free-длина округляется до такта.
static void test_quantize() {
  const uint64_t BAR = 96000;  // 120 уд/мин, 4/4
  {
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 1;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BAR / 2);
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 1);  // ждёт такта
    s.run(BAR / 2);
    CHECK(s.v().state == 2);
    s.run(BAR);
    CHECK(s.v().state == 3);
    CHECK(std::fabs(s.v().len_s - 2.0f) < 1e-3f);
  }
  {  // Free-длина: отжали на 1,3 такта -> такт доигрывается, цикл 2 такта
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);                      // ждём черту
    CHECK(s.v().state == 2);
    s.run((uint64_t)(BAR * 1.3));
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 2);         // ещё пишет
    s.run((uint64_t)(BAR * 0.7));
    CHECK(s.v().state == 3 && std::fabs(s.v().len_s - 4.0f) < 1e-3f);
  }
  {  // отжали на 100 мс позже черты 1-го такта -> цикл 1 такт, сразу играет
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);
    s.run(BAR + 4800);
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 3 && std::fabs(s.v().len_s - 2.0f) < 1e-3f);
    s.run(BAR);
    CHECK(s.max_step(0, s.t) < 0.03f);
  }
  {  // то же в ROP: после нажатия посреди такта пишет до черты, потом овердаб
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.rop = true;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);
    s.run((uint64_t)(BAR * 1.3));
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 2);
    s.run((uint64_t)(BAR * 0.7));
    CHECK(s.v().state == 4 && std::fabs(s.v().len_s - 4.0f) < 1e-3f);
  }
  {  // транспорт стоит, лупов нет: запись сразу
    Sim s(64, true);
    s.ti.rolling = false;
    s.run(BAR / 3);
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 2);
  }
}

// ROP: после закрытия цикла сразу овердаб, хвост ложится на начало; undo снимает этот слой.
static void test_rop() {
  Sim s;
  s.set.rop = true;
  s.apply();
  const uint64_t L = 48000;
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  CHECK(s.v().state == 4);
  s.in_l = sine(523.0, 0.2f);
  s.run(L / 2);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 3 && s.v().layers == 2);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(s.v().layers == 1);
  // после undo играет только основа (сравниваем середину цикла)
  uint64_t c = L + ((s.t - L) / L + 1) * L;
  s.run(c - s.t + L);
  float err = 0;
  for (uint64_t p = 2000; p < L - 2000; ++p)
    err = std::fmax(err, std::fabs(s.out_l[c + p] - 0.4f * (float)std::sin(PI2 * 220.0 * (double)p / SR)));
  CHECK(err < 1e-4f);
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// Reset: тап очищает и откатывается undo; удержание очищает всё и возвращает память.
static void test_reset() {
  Sim s;
  s.in_l = sine(220.0, 0.4f);
  uint32_t total = s.lp->free_chunks();
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);  // овердаб
  s.run(24000);
  s.cmd(Cmd::ResetTrack);
  s.run(BLOCK * 8);  // овердаб сначала плавно закрывается (вход ещё звучит), потом reset
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(4800);
  CHECK(s.v().state == 0);
  CHECK(rms(s.slice(s.t - 2400, 2400)) < 1e-6f);
  s.cmd(Cmd::Undo);
  s.run(48000);
  CHECK(s.v().state == 3);
  CHECK(rms(s.slice(s.t - 24000, 24000)) > 0.1f);
  // вторая дорожка, потом полный сброс
  s.cmd(Cmd::TrackNext);
  CHECK(s.lp->active() == 1);
  s.in_l = sine(330.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::ResetAll);
  s.run(4800);
  CHECK(s.v(0).state == 0 && s.v(1).state == 0 && !s.v(0).can_undo);
  CHECK(s.lp->free_chunks() == total);
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// Пул кончился: старые слои вливаются в основу, всё продолжает работать.
static void test_pool_full() {
  Sim s(32);
  const uint64_t L = 48000 * 3;  // ~9 кусков на слой, в 32 МБ влезает около 25 слоёв
  s.in_l = sine(200.0, 0.2f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  for (int k = 0; k < 40; ++k) {
    s.in_l = sine(250.0 + 7 * k, 0.01f);
    s.cmd(Cmd::Rec);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);
  }
  CHECK(s.v().state == 3);
  CHECK(s.v().layers > 5 && s.v().layers < 40);
  uint32_t before = s.v().layers;
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 16);
  CHECK(s.v().layers < before);  // undo нужна память под новую сумму: старые слои влились в основу
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// Quantize: reverse доигрывает такт, лента останавливается ровно на тактовой черте,
// после разворота позиция как при мгновенном развороте на черте (лупы остаются в долю).
static void test_reverse_quantized() {
  const uint64_t BAR = 96000;  // 120 уд/мин, 4/4
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);  // ждём такт, пишем 1 такт (+ хвост стыка): цикл начинается на черте
  CHECK(s.v().state == 3);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR / 3);  // середина такта
  const uint64_t L = BAR;
  const uint64_t next_bar = ((s.t / BAR) + 1) * BAR;
  s.cmd(Cmd::Reverse);
  const uint64_t n = 2400;  // 50 мс
  s.run(next_bar - n / 2 - s.t - BLOCK);  // до начала разворота — ещё вперёд
  CHECK(!s.v().reversed);
  s.run(BLOCK * 2);
  CHECK(s.v().reversed);  // разворот начался за n/2 до черты
  s.run(next_bar + n - s.t);  // разворот закончился
  // идеальная позиция: на черте 0 (цикл = такт), дальше назад
  const uint64_t want = (L - (s.t - next_bar) % L) % L;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)L) % L;
  int64_t diff = (int64_t)got - (int64_t)want;
  if (diff > (int64_t)L / 2) diff -= L;
  if (diff < -(int64_t)L / 2) diff += L;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  // повторное нажатие до черты отменяет разворот
  s.cmd(Cmd::Reverse);
  s.cmd(Cmd::Reverse);
  s.run(BAR * 2);
  CHECK(s.v().reversed);
}

// Записали луп, остановили транспорт (луп играет), Zynthian пошёл заново с первого такта:
// луп встаёт в фазу с позицией Zynthian через кроссфейд, без щелчка.
static void test_transport_restart_resync() {
  const uint64_t BAR = 96000;
  const double bpf = 120.0 / 60.0 / SR;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);  // запись с 1-го такта, 1 такт (+ хвост стыка)
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run((uint64_t)(BAR * 1.3));
  CHECK(s.v().state == 3);
  // стоп: позиция Zynthian замерла, луп играет дальше
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  s.run((uint64_t)(BAR * 0.45));
  // пуск с начала (доля 0)
  const uint64_t t_start = s.t;
  s.ti.rolling = true;
  s.beats_at = [t_start, bpf](uint64_t t) { return (double)(t - t_start) * bpf; };
  s.run(BLOCK * 20);
  // фаза: начало цикла на тактовой черте Zynthian
  const uint64_t want = (s.t - t_start) % BAR;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  s.wav("transport_restart");
}

// Транспорт остановили: лупы плавно затихают. Пустили: входят обратно в фазе, без щелчка.
static void test_transport_stop_start() {
  const uint64_t BAR = 96000;
  const double bpf = 120.0 / 60.0 / SR;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run((uint64_t)(BAR * 1.3));
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  const uint64_t t_stop = s.t;
  s.run(BAR / 2);
  CHECK(s.v().stopped && s.v().state == 3);
  CHECK(rms(s.slice(t_stop, 2400)) > 0.05f);           // затухает не сразу (200 мс)
  CHECK(rms(s.slice(s.t - 9600, 9600)) < 1e-6f);       // потом тишина
  s.cmd(Cmd::Rec);                                      // Rec на остановленной: молча ничего
  CHECK(s.v().state == 3);
  const uint64_t t_start = s.t;
  s.ti.rolling = true;
  s.beats_at = [t_start, bpf](uint64_t t) { return (double)(t - t_start) * bpf; };
  s.run(BAR / 2);
  CHECK(!s.v().stopped);
  CHECK(rms(s.slice(s.t - 9600, 9600)) > 0.1f);        // снова играет
  const uint64_t want = (s.t - t_start) % BAR;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  s.wav("transport_stop_start");
}

// Play/Stop: стоп на границе такта (транспорт идёт дальше), старт с начала такта в фазе.
static void test_play_stop() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR / 4);                         // четверть такта
  s.cmd(Cmd::PlayStop);
  CHECK(s.lp->playstop_pending() == 1);
  s.run(BAR / 2);                         // ещё до черты: играет
  CHECK(!s.v().stopped && rms(s.slice(s.t - 4800, 4800)) > 0.1f);
  s.run(BAR / 2);                         // черта прошла, 200 мс затухания прошли
  CHECK(s.v().stopped && s.lp->user_stopped());
  CHECK(rms(s.slice(s.t - 4800, 4800)) < 1e-6f);
  s.cmd(Cmd::PlayStop);
  s.cmd(Cmd::PlayStop);                   // отмена до черты
  s.run(BAR * 2);
  CHECK(s.v().stopped);
  s.cmd(Cmd::PlayStop);
  s.run(BAR + BLOCK * 4);                 // старт на черте
  CHECK(!s.v().stopped && rms(s.slice(s.t - 4800, 4800)) > 0.1f);
  const uint64_t want = s.t % BAR;        // цикл = такт, сетка от t = 0
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  // остановили педалью — пуск транспорта не включает
  s.cmd(Cmd::PlayStop);
  s.run(BAR * 2);
  CHECK(s.v().stopped);
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  s.run(BAR / 2);
  const uint64_t t0 = s.t;
  s.ti.rolling = true;
  s.beats_at = [t0](uint64_t t) { return (double)(t - t0) * 120.0 / 60.0 / SR; };
  s.run(BAR);
  CHECK(s.v().stopped);
  s.wav("play_stop");
}

// Пики для волн: после записи — волна суммы, после овердабов — слои (новые первыми),
// во время овердаба волна слоя растёт вживую; undo убирает слой.
static void test_peaks() {
  Sim s;
  const uint64_t L = 48000;
  const PeakTrack& pk = s.peaks->t[0];
  s.in_l = [](uint64_t t) { return t < 24000 ? 0.5f * (float)std::sin(PI2 * 220.0 * (double)t / SR) : 0.0f; };
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(pk.seq % 2 == 0 && pk.seq > 0 && pk.nlayers == 0);
  // первая половина цикла — синус 0,5 (64 из 127), вторая — тишина
  int mx = 0, mn = 0;
  for (int c = 50; c < 200; ++c) {
    mx = std::max(mx, (int)pk.sum.mx[c]);
    mn = std::min(mn, (int)pk.sum.mn[c]);
  }
  CHECK(mx >= 60 && mx <= 66 && mn <= -60);
  CHECK(pk.sum.mx[400] == 0 && pk.sum.mn[400] == 0);
  // овердаб во второй половине: живая волна слоя растёт
  s.in_l = sine(330.0, 0.25f);
  s.run(L - (s.t % L) + L / 2);  // до середины цикла
  s.cmd(Cmd::Rec);
  uint32_t live0 = pk.live;
  s.run(L / 4);
  CHECK(pk.dub_active == 1 && pk.live > live0);
  CHECK(pk.dub.mx[320] >= 28 && pk.dub.mx[100] == 0);
  s.run(L / 4);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BLOCK * 8);
  CHECK(pk.dub_active == 0 && pk.nlayers == 1 && pk.layer_no[0] == 1);
  CHECK(pk.layer[0].mx[400] >= 28 && pk.layer[0].mx[100] == 0);
  CHECK(pk.sum.mx[400] >= 28);
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(pk.nlayers == 0 && pk.sum.mx[400] == 0);
  s.cmd(Cmd::ResetTrack);
  s.run(BLOCK * 8);
  CHECK(pk.sum.mx[100] == 0);
}

// Парты и Lock (этап 2).
static void test_parts_lock() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = s.set.length[1] = 1;
  s.apply();
  auto at = [&](double bars) { s.run((uint64_t)std::llround(bars * BAR) - s.t); };
  // дорожка 1: синус 180 Гц, 1 такт с 1-го такта; Lock
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  at(2.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.cmd(Cmd::Lock);
  CHECK(s.v(0).locked);
  // дорожка 2: синус 300 Гц, со 2-го такта
  s.cmd(Cmd::TrackNext);
  s.in_l = sine(300.0, 0.3f);
  s.cmd(Cmd::Rec);
  at(3.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  at(4.0);
  CHECK(s.v(0).state == 3 && s.v(1).state == 3 && s.lp->parts_mask() == 1u);
  at(5.0);
  const uint32_t free_before = s.lp->free_chunks();
  // переход в пустой парт 2 посреди такта: ждёт черты
  at(5.25);
  s.cmd(Cmd::PartNext);
  CHECK(s.lp->pending_part() == 1 && s.lp->part() == 0);
  at(5.9);
  CHECK(s.lp->part() == 0);
  at(6.0 + 0.01);
  CHECK(s.lp->part() == 1 && s.lp->pending_part() == -1);
  const std::vector<float> ref0 = s.slice(5 * BAR, BAR);  // звук парта 1 за последний такт перед переходом
  CHECK(s.v(0).state == 3 && s.v(0).locked);  // Lock-дорожка перешла
  CHECK(s.v(1).state == 0);                   // остальные — нет
  CHECK(s.lp->active() == 1);                 // активная — первая свободная
  CHECK(s.lp->free_chunks() == free_before);  // копия не тратит память
  at(6.5);
  // звучит только дорожка 1, в той же фазе (позиция цикла = t % BAR), без скачка
  float err = 0;
  for (uint64_t t = (uint64_t)(6.3 * BAR); t < (uint64_t)(6.5 * BAR); ++t) {
    const double p = (double)(BAR + t % BAR);
    err = std::fmax(err, std::fabs(s.out_l[t] - 0.4f * (float)std::sin(PI2 * 180.0 * p / SR)));
  }
  CHECK(err < 1e-4f);
  CHECK(s.max_step(5 * BAR, s.t) < 0.03f);
  // овердаб на копии: память тратится только на изменённые куски, старый парт не меняется
  s.cmd(Cmd::TrackPrev);
  CHECK(s.lp->active() == 0);
  at(7.0);
  s.in_l = sine(523.0, 0.2f);
  s.cmd(Cmd::Rec);
  at(8.0);
  s.cmd(Cmd::Rec);
  at(8.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  CHECK(s.v(0).state == 3 && s.v(0).layers == 1);
  CHECK(s.lp->free_chunks() < free_before);
  CHECK(s.lp->parts_mask() == 3u);
  // назад в записанный парт 1: на черте, с начала цикла, без овердаба копии
  at(8.5);
  s.cmd(Cmd::PartPrev);
  at(9.0 + 0.01);
  CHECK(s.lp->part() == 0 && s.v(0).state == 3 && s.v(1).state == 3);
  at(11.0);
  CHECK(rms_diff(s.slice(10 * BAR, BAR), ref0) < 1e-4f);
  CHECK(s.max_step(8 * BAR, s.t) < 0.05f);
  // повторный тап переносит цель, вернулись на текущий — отмена
  s.cmd(Cmd::PartNext);
  s.cmd(Cmd::PartNext);
  CHECK(s.lp->pending_part() == 2);
  s.cmd(Cmd::PartPrev);
  CHECK(s.lp->pending_part() == 1);
  s.cmd(Cmd::PartPrev);
  CHECK(s.lp->pending_part() == -1);
  s.wav("parts_lock");
}

// Из пустого парта — сразу; запись закрывается на границе перехода; Lock во время овердаба.
static void test_parts_edges() {
  const uint64_t BAR = 96000;
  {
    Sim s(64, true);
    s.cmd(Cmd::PartNext);
    CHECK(s.lp->part() == 1 && s.lp->pending_part() == -1);
    s.cmd(Cmd::PartPrev);
    CHECK(s.lp->part() == 0);
  }
  {
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR + BAR / 2);  // пишет с 1-го такта
    s.cmd(Cmd::PartNext);
    CHECK(s.lp->pending_part() == 1);
    s.run(BAR / 2 + BLOCK * 2);
    CHECK(s.lp->part() == 1);
    s.cmd(Cmd::PartPrev);  // парт 2 пустой — сразу назад
    CHECK(s.lp->part() == 0 && s.v(0).state == 3 && std::fabs(s.v(0).len_s - 2.0f) < 1e-3f);
  }
  {  // Lock-дорожка в овердабе: копия после затухания овердаба, без щелчка
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 1;
    s.apply();
    s.in_l = sine(180.0, 0.4f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(2 * BAR + BLOCK * 8);
    s.cmd(Cmd::Lock);
    s.in_l = sine(400.0, 0.2f);
    s.run(BAR / 4);
    s.cmd(Cmd::Rec);  // овердаб
    s.run(BAR / 2);
    s.cmd(Cmd::PartNext);
    s.run(BAR / 4 + BLOCK * 8);
    s.in_l = [](uint64_t) { return 0.0f; };
    CHECK(s.lp->part() == 1 && s.v(0).state == 3 && s.v(0).locked);
    s.run(BAR);
    CHECK(rms(s.slice(s.t - BAR / 2, BAR / 2)) > 0.1f);
    CHECK(s.max_step(2 * BAR, s.t) < 0.05f);
  }
}

int main() {
  test_record_play();
  test_overdubs_undo();
  test_mute();
  test_reverse();
  test_reverse_quantized();
  test_transport_restart_resync();
  test_transport_stop_start();
  test_play_stop();
  test_peaks();
  test_parts_lock();
  test_parts_edges();
  test_quantize();
  test_rop();
  test_reset();
  test_pool_full();
  std::printf("looper: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
