// Звуковое ядро лупера (этап 1): 4 стерео-дорожки, запись, овердабы, undo/redo по слою,
// mute, reverse как лента, Free и Quantize, длина дорожки, RPO и ROP, Reset.
// Без LV2. process() и всё с пометкой RT вызываются из аудиопотока: без выделения памяти,
// блокировок и системных вызовов. do_work() — из рабочего потока (LV2 worker).
#pragma once
#include <cstdint>

#include "pool.h"

namespace alp {

constexpr int kTracks = 4;
constexpr uint32_t kMaxUndo = 1024;  // записей undo на дорожку (дальше старые вливаются в основу)

enum class Cmd : uint8_t { Rec, Undo, Redo, ResetTrack, ResetAll, Mute, Reverse, TrackNext, TrackPrev };

// Настройки (виджет настроек). Меняются в любой момент, применяются со следующего блока.
struct Settings {
  bool rop = false;           // false: RPO, true: ROP
  bool quantize = true;       // false: Free
  int length[kTracks] = {0, 0, 0, 0};  // тактов, 0 = Free
  float fade_out_ms = 200.0f;  // mute
  float fade_in_ms = 200.0f;   // unmute
  float reverse_ms = 50.0f;    // время разворота
  int mute_q = 2;             // 0 сразу, 1 доля, 2 такт, 3 конец цикла
  float dry = 1.0f;           // сухой сигнал на выходе
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
  bool muted, reversed, can_undo, can_redo;
  float pos;         // 0..1 доля цикла
  float len_s;       // длина цикла, с
  uint32_t layers;   // слоёв в undo
};

// Задание рабочему потоку: dst = src ± layer (подготовка undo/redo).
struct WorkMsg {
  uint8_t op;  // 1 вычесть слой (undo), 2 прибавить (redo)
  uint8_t track;
  int32_t src, layer, dst;
  uint64_t len;
};
struct WorkResp {
  uint8_t op;
  uint8_t track;
};

class Looper {
 public:
  // Не RT. pool_bytes — размер пула (по ТЗ 2 ГБ).
  bool init(double sample_rate, size_t pool_bytes);
  bool ok() const { return pool_.ok(); }

  // RT
  void set_settings(const Settings& s) { set_ = s; }
  // Время текущего блока. Вызывать до command(), чтобы команды видели свежий транспорт.
  void set_time(const TimeInfo& t);
  const Settings& settings() const { return set_; }
  void command(Cmd c);
  void process(const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n, const TimeInfo& t);
  bool take_work(WorkMsg& m);
  void work_done(const WorkResp& r);

  // рабочий поток
  void do_work(const WorkMsg& m, WorkResp& r);

  // RT: состояние
  int active() const { return active_; }
  TrackView view(int i) const;
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
    float gain = 1.0f;
    bool busy = false;       // рабочий поток считает undo/redo этой дорожки
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
  void leave_track(int ti);

  // undo/redo и память
  void push_undo(Track& t, Entry e);
  void clear_redo(Track& t);
  void free_buf_safe(Track& t, int32_t b);
  void drop_entry_bufs(Track& t, const Entry& e);
  bool reserve(uint32_t need, int prefer);
  void swap_to(int ti, int32_t new_sum, bool free_old);
  bool post_job(int ti, uint8_t op, int32_t layer);

  // звук
  void track_sample(int ti, float il, float ir, float& ol, float& orr);
  void read_sum(const Track& t, int32_t b, double pos, float& l, float& r) const;
  void write_add(int32_t b, uint64_t p, float l, float r);
  void seam_write(Track& t, float l, float r);
  void dub_write(int ti, uint64_t p, float l, float r);
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
  double beats_ = 0.0, bpf_ = 0.0, anchor_ = 0.0;
  int bpb_ = 4;
  bool rolling_ = false;
  // рабочий поток: одно задание за раз
  bool job_pending_ = false, job_running_ = false;
  WorkMsg job_{};
  bool reset_all_pending_ = false;
};

}  // namespace alp
