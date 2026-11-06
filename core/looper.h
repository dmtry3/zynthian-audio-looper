// Звуковое ядро лупера: 4 парта по 4 стерео-дорожки, запись, овердабы, undo/redo по слою,
// mute, reverse как лента, Free и Quantize, длина дорожки, RPO и ROP, Reset, парты и Lock.
// Без LV2. process() и всё с пометкой RT вызываются из аудиопотока: без выделения памяти,
// блокировок и системных вызовов. do_work() — из рабочего потока (LV2 worker).
#pragma once
#include <cstdint>

#include "peaks.h"
#include "pool.h"

namespace alp {

constexpr int kSlots = 4;                 // дорожек в парте
constexpr int kParts = 4;
constexpr int kTracks = kSlots * kParts;  // дорожка k принадлежит парту k / kSlots
constexpr uint32_t kMaxUndo = 1024;  // записей undo на дорожку (дальше старые вливаются в основу)

enum class Cmd : uint8_t {
  Rec, Undo, Redo, ResetTrack, ResetAll, Mute, Reverse, TrackNext, TrackPrev, PlayStop, PartNext, PartPrev, Lock
};

// Настройки (виджет настроек). Меняются в любой момент, применяются со следующего блока.
struct Settings {
  bool rop = false;           // false: RPO, true: ROP
  bool quantize = true;       // false: Free
  int length[kSlots] = {0, 0, 0, 0};  // тактов, 0 = Free (по месту дорожки в парте)
  float fade_out_ms = 200.0f;  // mute
  float fade_in_ms = 200.0f;   // unmute
  float reverse_ms = 50.0f;    // время разворота
  int mute_q = 2;             // 0 сразу, 1 доля, 2 такт, 3 конец цикла
  int part_q = 3;             // смена парта: 0 сразу, 1 доля, 2 такт, 3 конец цикла (самого длинного лупа)
  float tempo_glide_ms = 500.0f;  // плавная смена темпа (следование темпу)
  int spd_snap = 0;               // привязка ручной скорости: 0 свободно, 1 полутоны, 2 октавы
  float spd_glide_ms = 150.0f;    // плавный ход ручной скорости
};

constexpr float kMinSpeed = 0.25f, kMaxSpeed = 2.0f;  // ручная скорость дорожки, как у ленты

// Микшер дорожки. Режим «Стерео»: панорама как баланс с постоянной мощностью.
// «Раздельно» (L и R — разные инструменты): в центре L слева, R справа, ничего не смешивается;
// к краю противоположный канал плавно уходит в сторону панорамы.
constexpr float kMinDb = -60.0f;  // ниже — тишина (−∞)
struct Mix {
  float level_db = 0.0f;  // −60 (−∞) .. +6
  float pan = 0.0f;       // −1 .. 1
  int mode = 0;           // 0 стерео, 1 раздельно
};

// Время блока: доли считаются от начала (абсолютные), растут на beats_per_frame за кадр.
struct TimeInfo {
  double beats = 0.0;
  double beats_per_frame = 120.0 / 60.0 / 48000.0;
  int bpb = 4;
  bool rolling = false;
};

enum class TState : uint8_t { Empty, Armed, Rec, Closing, Play, Dub };

// Для выходных портов и виджета.
struct TrackView {
  uint8_t state;     // 0 пусто, 1 ждёт такта, 2 запись, 3 играет, 4 овердаб
  bool muted, reversed, can_undo, can_redo, stopped, locked;
  float pos;         // 0..1 доля цикла
  float speed;       // ручная скорость сейчас (с плавным ходом)
  float espeed;      // итоговая: ручная × следование темпу × автоподстройка
  float len_s;       // длина цикла, с
  uint32_t layers;   // слоёв в undo
};

// Задание рабочему потоку: dst = src ± layer (подготовка undo/redo).
// Или (op 3) пересчитать пики дорожки для волн виджета.
struct WorkMsg {
  uint8_t op;  // 1 вычесть слой (undo), 2 прибавить (redo), 3 пики
  uint8_t track;
  uint8_t nl;  // op 3: слоёв в lay[]
  int32_t src, layer, dst;
  uint64_t len;
  uint32_t gen;
  int32_t lay[kPeakLayers];
  uint32_t lno[kPeakLayers];
};
struct WorkResp {
  uint8_t op;
  uint8_t track;
  uint32_t gen;
};

class Looper {
 public:
  // Не RT. pool_bytes — размер пула (по ТЗ 2 ГБ).
  bool init(double sample_rate, size_t pool_bytes);
  bool ok() const { return pool_.ok(); }

  // RT
  void set_settings(const Settings& s) { set_ = s; }
  // Время текущего блока. Вызывать до command(), чтобы команды видели свежий транспорт.
  // Транспорт идёт: сетка = позиция Zynthian. Стоит: лупер считает доли сам по темпу.
  void set_time(const TimeInfo& t);
  const Settings& settings() const { return set_; }
  void command(Cmd c);
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n, const TimeInfo& t);
  bool take_work(WorkMsg& m);
  void work_done(const WorkResp& r);
  void cancel_work(const WorkMsg& m);  // хост не принял задание: вернуть всё как было
  // Память для пиков волн (общая с виджетом). nullptr — пики не считаются.
  void set_peaks(PeakShm* p) { peaks_ = p; }

  // рабочий поток
  void do_work(const WorkMsg& m, WorkResp& r);

  // RT: состояние (view и active — по месту дорожки в текущем парте)
  int active() const { return active_; }
  int active_index() const { return idx(active_); }  // номер активной дорожки среди всех 16
  int part() const { return cur_part_; }
  int pending_part() const { return pend_part_; }
  bool rec_after_switch() const { return rec_after_switch_; }
  uint32_t parts_mask() const;  // бит p: в парте p что-то записано
  bool user_stopped() const { return user_stop_; }
  int playstop_pending() const { return ps_pending_; }
  double grid_beats() const { return beats_; }  // доли по сетке лупера (при стоящем транспорте — от первой записи)
  TrackView view(int i) const;
  // микшер (RT): k — дорожка 0..15; mix_epoch растёт, когда ядро меняет микшер само
  // (сброс удержанием Reset, Lock-копия) — обёртка сообщает новые значения хосту
  void set_mix(int k, const Mix& m) { mix_[k] = m; }
  void mute_slot(int slot);  // mute дорожки текущего парта (кнопка M в виджете Mixer)
  // ручная скорость дорожки k (0..15): цель, к ней плавный ход; сброс — к 1× и в фазу с тактом
  void set_speed(int k, float s) { tr_[k].mspd = s < kMinSpeed ? kMinSpeed : (s > kMaxSpeed ? kMaxSpeed : s); }
  float speed(int k) const { return tr_[k].mspd; }
  void speed_reset(int k);
  const Mix& mix(int k) const { return mix_[k]; }
  void set_master_db(float db) { master_db_ = db; }
  float master_db() const { return master_db_; }
  void set_dry_db(float db) { dry_db_ = db; }
  float dry_db() const { return dry_db_; }
  void set_limiter(bool on) { lim_on_ = on; }
  bool limiter() const { return lim_on_; }
  uint32_t mix_epoch() const { return mix_epoch_; }
  float peak(int slot) const { return tpeak_[idx(slot)]; }  // пик дорожки текущего парта, 0..1+
  float master_peak(int ch) const { return mpeak_[ch & 1]; }
  float pool_fill() const;
  uint32_t free_chunks() const { return pool_.free_chunks(); }
  uint32_t total_chunks() const { return pool_.total_chunks(); }

 private:
  enum : uint8_t { U_DELTA = 1, U_SNAP = 2 };
  struct Entry {
    uint8_t type;
    int32_t buf;  // DELTA: слой; SNAP: сумма, на которую переключиться (-1 = пусто)
  };
  struct Track {
    TState st = TState::Empty;
    int32_t sum = -1;        // что играет (сумма всех слоёв)
    uint64_t len = 0;        // длина цикла, кадров
    // кроссфейд со старой суммы (undo, redo, reset, старт)
    int32_t xf_from = -1;
    bool xf_owned = false;   // вернуть xf_from в пул после кроссфейда
    uint32_t xf_pos = 0, xf_len = 0;
    // undo: кольцо, redo: стек
    Entry undo[kMaxUndo];
    uint32_t undo_lo = 0, undo_n = 0;
    Entry redo[kMaxUndo];
    uint32_t redo_n = 0;
    // воспроизведение
    double pos = 0.0;
    double rate = 1.0;
    int dir = 1;
    double rate_step = 0.0;
    double xpos = 0.0;       // позиция чтения xf_from (при перескоке фазы отличается от pos)
    // фаза относительно сетки (Quantize): позиция 0 цикла приходится на долю b0 + k*loop_beats
    bool synced = false;
    bool rec_q = false;
    double b0 = 0.0, loop_beats = 0.0, rec_bpf = 0.0;
    // скорость: ручная цель, ручная сейчас (плавный ход), итоговая на этот блок
    float mspd = 1.0f;
    double mcur = 1.0;
    double spd = 1.0;
    double tspd = 1.0;         // следование темпу (с подстройкой фазы), без ручной
    bool phase_reset = false;  // сброс скорости: когда дойдёт до 1×, встать в фазу с тактом
    bool phase_wait = false;   // ждём границы (MUTE AT)
    // овердаб с пересчётом под скорость: прошлая позиция головки и прошлый вход
    double wprev = 0.0;
    float wl_prev = 0.0f, wr_prev = 0.0f;
    bool wfirst = true;
    int64_t rev_wait = -1;   // Quantize: кадров до начала разворота (-1 = не ждём)
    double rev_n = 0.0;      // длина разворота, кадров
    // запись основы
    int32_t rec = -1;
    uint64_t rec_frames = 0, rec_target = 0;
    // стык цикла и овердаб
    uint64_t seam_t = 0;     // кадров после закрытия цикла; < xf — пишется хвост
    int32_t layer = -1;
    uint64_t dub_t = 0;      // кадров с начала овердаба
    int64_t dub_end = -1;    // >= 0: идёт затухание конца овердаба
    bool undo_after_dub = false;
    bool reset_after_dub = false;
    // mute
    bool muted = false, mute_pending = false;
    bool stopped = false;    // транспорт Zynthian остановлен: дорожка затихла и ждёт пуска
    bool ustop = false;      // остановлена педалью Play/Stop: ждёт повторного нажатия
    float sgain = 1.0f;      // огибающая остановки по транспорту
    float gain = 1.0f;
    bool busy = false;       // рабочий поток считает undo/redo этой дорожки
    // пики для волн: поколение суммы, нужен пересчёт, пересчёт идёт, аудиопоток дорисовал
    uint32_t pgen = 0;
    bool pdirty = false, pinflight = false, plive = false;
    // Lock: пойдёт в следующий пустой парт; lock_to — копия ждёт конца овердаба
    bool locked = false;
    int lock_to = -1;
    bool lock_ready = false;  // овердаб затух: копия делается в начале следующего кадра
  };

  // команды
  void rec_press(int ti);
  void undo(int ti);
  void redo(int ti);
  void reset_track(int ti);
  void reset_all_now();
  void start_rec(int ti);
  void cancel_rec(int ti);
  void close_loop(int ti, uint64_t L);
  void request_close(int ti);
  void start_dub(int ti);
  void end_dub(int ti);
  void finish_dub(int ti);
  void apply_mute(int ti);
  void start_reverse(Track& t, double n);
  void resync(int ti);
  void mark_peaks(Track& t) {
    ++t.pgen;
    t.pdirty = true;
  }
  void live_peaks(int ti, uint64_t p);
  void transport_stop();
  void transport_start();
  void wake(int ti);
  void apply_playstop(bool stop);
  void leave_track(int ti);
  // парты
  int idx(int slot) const { return cur_part_ * kSlots + slot; }
  bool in_cur(int k) const { return k / kSlots == cur_part_; }
  bool part_has_content(int p) const;
  void switch_part(int p);
  void clone_into(int from, int to);
  void clear_track(int k);
  void default_locks();

  // undo/redo и память
  void push_undo(Track& t, Entry e);
  void clear_redo(Track& t);
  void free_buf_safe(Track& t, int32_t b);
  void drop_entry_bufs(Track& t, const Entry& e);
  bool reserve(uint32_t need, int prefer);
  void swap_to(int ti, int32_t new_sum, bool free_old);
  bool post_job(int ti, uint8_t op, int32_t layer);

  // звук
  void track_sample(int ti, uint32_t i, float il, float ir, float& ol, float& orr);
  void reset_mixer();
  static void mix_matrix(const Mix& m, float out[4]);
  void read_sum(const Track& t, int32_t b, double pos, float& l, float& r) const;
  void write_add(int32_t b, uint64_t p, float l, float r);
  void seam_write(Track& t, float l, float r);
  void dub_write(int ti, uint64_t p, float l, float r);
  float dub_env(Track& t);
  bool dub_put(int ti, uint64_t p, float l, float r);
  void dub_resampled(int ti, float il, float ir, double step);
  void update_speeds(uint32_t n);
  float snap_speed(float s) const;
  void on_grid(bool beat, bool bar);
  bool has_content() const;
  uint64_t bar_frames() const;

  Pool pool_;
  Settings set_;
  double sr_ = 48000.0;
  uint32_t xf_ = 480;          // внутренний кроссфейд стыка, овердаба, undo (10 мс)
  Track tr_[kTracks];
  int active_ = 0;
  // сетка
  double beats_ = 0.0, bpf_ = 0.0;  // сетка: при стоящем транспорте лупер считает сам
  int bpb_ = 4;
  bool rolling_ = false;
  bool time_seen_ = false;
  bool user_stop_ = false;  // лупы остановлены педалью Play/Stop
  bool tstop_ = false;      // лупы затихли из-за остановки транспорта
  int cur_part_ = 0;
  int pend_part_ = -1;      // парт, ждущий границы (-1 нет)
  bool clones_pending_ = false;
  bool rec_after_switch_ = false;
  bool part_due_ = false;     // конец цикла: переход в начале следующего кадра
  int master_ = -1;           // самая длинная играющая дорожка текущего парта (её цикл — «конец цикла»)  // Rec нажат, пока переход ждёт: запись в новом парте на границе
  int ps_pending_ = 0;      // ждёт границы: 1 — стоп, 2 — старт
  // рабочий поток: одно задание за раз
  bool job_pending_ = false, job_running_ = false;
  WorkMsg job_{};
  bool reset_all_pending_ = false;
  PeakShm* peaks_ = nullptr;
  // микшер: целевые значения, коэффициенты LL RL LR RR в начале блока и шаг за кадр
  Mix mix_[kTracks];
  float mcur_[kTracks][4] = {};
  float mstep_[kTracks][4] = {};
  float bmax_[kTracks] = {};
  float tpeak_[kTracks] = {};
  float master_db_ = 0.0f, dry_db_ = 0.0f;
  float master_g_ = 1.0f, dry_g_ = 1.0f;
  bool lim_on_ = true;
  float lim_env_ = 0.0f;
  float mpeak_[2] = {};
  uint32_t mix_epoch_ = 0;
};

}  // namespace alp
