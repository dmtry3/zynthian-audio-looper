// Темп и позиция: от Zynthian (LV2 time:Position) и запасной путь по MIDI-клоку.
// Без LV2, без выделения памяти: безопасно вызывать из аудиопотока.
#pragma once
#include <cmath>
#include <cstdint>

namespace alp {

// Сообщение time:Position. Поля, которых не было в сообщении, помечены has_* = false.
struct PositionMsg {
  bool has_bpm = false, has_bpb = false, has_speed = false, has_pos = false, has_frame = false;
  double bpm = 0, bpb = 0, speed = 0;
  int64_t frame = 0;             // кадр транспорта JACK (jalv шлёт его всегда)
  double bar = 0, bar_beat = 0;  // такт от 0, доля внутри такта от 0 (с дробной частью)
};

// Позиция транспорта Zynthian в долях. Между сообщениями time:Position идёт сама по темпу:
// jalv шлёт позицию не каждый блок, а при изменениях. Пока транспорт стоит, позиция стоит.
// Если в сообщении нет такта и доли (только кадр), позиция досчитывается по сдвигу кадра.
class TempoClock {
 public:
  void set_sample_rate(double sr) { sr_ = sr > 0 ? sr : 48000.0; }

  // Сообщение time:Position от Zynthian (считается в messages()).
  void on_position(const PositionMsg& m) {
    ++messages_;
    apply_external(m);
  }

  // То же без счёта: для запасного пути по MIDI-клоку.
  void apply_external(const PositionMsg& m) {
    if (m.has_bpm && m.bpm >= 20.0 && m.bpm <= 400.0) bpm_ = m.bpm;
    if (m.has_bpb) {
      int v = (int)std::lround(m.bpb);
      if (v >= 1 && v <= 16) {
        bpb_ = v;
        bpb_seen_ = true;
      }
    }
    if (m.has_speed) rolling_ = m.speed != 0.0;
    if (m.has_pos && m.bar >= 0 && m.bar_beat >= 0) {
      beats_ = m.bar * bpb_ + m.bar_beat;
    } else if (m.has_frame && have_frame_) {
      beats_ += (double)(m.frame - frame_) * bpm_ / 60.0 / sr_;
      if (beats_ < 0) beats_ = 0;
    }
    if (m.has_frame) {
      frame_ = m.frame;
      have_frame_ = true;
    }
  }

  void advance(uint32_t frames) {
    if (!rolling_) return;
    beats_ += bpm_ / 60.0 * frames / sr_;
    frame_ += frames;
  }

  // Долей в такте из настройки: действует, пока Zynthian не прислал свой размер такта.
  void set_default_bpb(int v) {
    if (v < 1 || v > 16) return;
    if (!bpb_seen_ && v != bpb_) {
      const int64_t b = bar();  // счёт не прыгает: тот же такт и доля с начала такта
      const double bb = beats_ - (double)b * bpb_;
      bpb_ = v;
      beats_ = (double)b * bpb_ + (bb < bpb_ ? bb : 0.0);
    }
  }
  bool bpb_from_host() const { return bpb_seen_; }

  double bpm() const { return bpm_; }
  int bpb() const { return bpb_; }
  bool rolling() const { return rolling_; }
  uint32_t messages() const { return messages_; }
  double beats() const { return beats_; }
  int64_t bar() const { return (int64_t)std::floor(beats_ / bpb_); }
  double bar_beat() const { return beats_ - (double)bar() * bpb_; }  // 0 .. bpb

 private:
  double sr_ = 48000.0;
  double bpm_ = 120.0;
  int bpb_ = 4;
  bool bpb_seen_ = false;  // Zynthian прислал размер такта
  bool rolling_ = false;
  uint32_t messages_ = 0;
  double beats_ = 0.0;
  int64_t frame_ = 0;
  bool have_frame_ = false;
};

// MIDI-клок: 24 тика на долю. Темп по среднему интервалу за последнюю долю.
class MidiClock {
 public:
  static constexpr int kPpq = 24;

  void set_sample_rate(double sr) { sr_ = sr > 0 ? sr : 48000.0; }

  // t: время тика в сэмплах от старта плагина.
  void on_tick(uint64_t t) {
    if (have_last_) {
      uint64_t d = t - last_;
      if (d > 0 && d < (uint64_t)sr_) {  // интервал больше секунды = клок прерывался
        sum_ -= ring_[pos_];
        ring_[pos_] = d;
        sum_ += d;
        pos_ = (pos_ + 1) % kPpq;
        if (count_ < kPpq) ++count_;
      } else {
        reset_intervals();
      }
    }
    last_ = t;
    have_last_ = true;
    if (running_) ++ticks_;
  }
  void on_start() { running_ = true; ticks_ = 0; }
  void on_continue() { running_ = true; }
  void on_stop() { running_ = false; }

  // Клок живой, если последний тик был не раньше полсекунды назад.
  bool active(uint64_t now) const { return have_last_ && now - last_ < (uint64_t)(sr_ / 2); }
  double bpm() const {
    if (count_ < kPpq / 4 || sum_ == 0) return 0.0;
    return 60.0 * sr_ * count_ / ((double)kPpq * (double)sum_);
  }
  bool running() const { return running_; }
  double beats() const { return (double)ticks_ / kPpq; }

 private:
  void reset_intervals() {
    for (auto& r : ring_) r = 0;
    sum_ = 0;
    pos_ = 0;
    count_ = 0;
  }

  double sr_ = 48000.0;
  uint64_t ring_[kPpq] = {};
  uint64_t sum_ = 0;
  int pos_ = 0, count_ = 0;
  uint64_t last_ = 0;
  bool have_last_ = false;
  bool running_ = false;
  uint64_t ticks_ = 0;
};

}  // namespace alp
