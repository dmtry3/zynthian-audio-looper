#include "looper.h"

#include <cmath>

namespace alp {

namespace {
constexpr uint32_t kReserve = 4;     // кусков про запас, чтобы запись не упёрлась в ноль
constexpr uint64_t kNoSeam = ~0ull;
constexpr double kLatePressS = 0.2;
constexpr double kRecColS = 0.02;  // волна записи: секунд в колонке до первого слияния
constexpr double kMixSmoothS = 0.02;   // сглаживание уровня и панорамы
constexpr float kLimThr = 0.891f;      // ограничитель мастера: −1 дБ
constexpr float kLimRelS = 0.15f;      // отпускание ограничителя
constexpr float kPeakFallDbS = 20.0f;  // спад пиковых индикаторов, дБ/с

inline float db_gain(float db) { return db <= kMinDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

// Мягкое ограничение у 0 дБ: подстраховка после ограничителя.
inline float soft_clip(float y) {
  const float k = 0.95f, a = std::fabs(y);
  if (a <= k) return y;
  const float v = k + (1.0f - k) * std::tanh((a - k) / (1.0f - k));
  return y < 0 ? -v : v;
}  // опоздание нажатия после черты, которое ещё считается «на черте»

inline uint64_t wrap_index(int64_t i, uint64_t L) {
  int64_t m = i % (int64_t)L;
  return (uint64_t)(m < 0 ? m + (int64_t)L : m);
}
}  // namespace

bool Looper::init(double sample_rate, size_t pool_bytes) {
  sr_ = sample_rate > 0 ? sample_rate : 48000.0;
  xf_ = (uint32_t)std::lround(sr_ * 0.010);
  if (xf_ < 16) xf_ = 16;
  for (auto& t : tr_) t.seam_t = kNoSeam;
  default_locks();
  for (int k = 0; k < kTracks; ++k) mix_matrix(mix_[k], mcur_[k]);
  // Если столько памяти не дали, пробуем меньше: плагин должен загрузиться.
  for (size_t b = pool_bytes; b >= ((size_t)32 << 20); b /= 2)
    if (pool_.init(b, 4096)) return true;
  return false;
}

// ---------------------------------------------------------------- команды

void Looper::command(Cmd c) {
  if (!pool_.ok()) return;
  const int a = idx(active_);
  Track& t = tr_[a];
  switch (c) {
    case Cmd::Rec:
      // переход ждёт границы: запись взводится в новом парте (повторное нажатие — отмена)
      if (pend_part_ >= 0) rec_after_switch_ = !rec_after_switch_;
      else rec_press(a);
      break;
    case Cmd::Undo: undo(a); break;
    case Cmd::Redo: redo(a); break;
    case Cmd::ResetTrack: reset_track(a); break;
    case Cmd::ResetAll:
      if (job_pending_ || job_running_) reset_all_pending_ = true;
      else reset_all_now();
      break;
    case Cmd::Mute: mute_slot(active_); break;
    case Cmd::Reverse: {
      if ((t.st != TState::Play && t.st != TState::Dub) || t.len == 0) break;
      if (t.rev_wait >= 0) {  // повторное нажатие до такта отменяет разворот
        t.rev_wait = -1;
        break;
      }
      double n = set_.reverse_ms * sr_ / 1000.0;
      if (n < sr_ * 0.005) n = sr_ * 0.005;  // не короче 5 мс
      if (!set_.quantize) {
        start_reverse(t, n);
        break;
      }
      // Quantize: такт доигрывается. Лента останавливается ровно на тактовой черте:
      // разворот начинается за n/2 до неё, и после него позиция та же, что при мгновенном
      // развороте на черте (вперёд уходим на n/4 и столько же возвращаемся).
      const double bf = bpb_ / (bpf_ > 1e-12 ? bpf_ : 2.0 / sr_);
      const double next_bar = (std::floor(beats_ / bpb_) + 1.0) * bpb_;
      double start = (next_bar - beats_) / (bpf_ > 1e-12 ? bpf_ : 2.0 / sr_) - n / 2.0;
      if (start < 0.0) start += bf;  // до черты меньше половины разворота: следующий такт
      t.rev_wait = (int64_t)std::llround(start);
      t.rev_n = n;
      break;
    }
    case Cmd::PlayStop: {
      if (ps_pending_) {  // повторное нажатие до границы отменяет
        ps_pending_ = 0;
        break;
      }
      const bool stop = !user_stop_;
      if (!set_.quantize || set_.mute_q == 0) apply_playstop(stop);
      else ps_pending_ = stop ? 1 : 2;
      break;
    }
    case Cmd::TrackNext:
    case Cmd::TrackPrev:
      leave_track(a);
      active_ = (active_ + (c == Cmd::TrackNext ? 1 : ns_ - 1)) % ns_;
      break;
    case Cmd::PartNext:
    case Cmd::PartPrev: {
      const int d = c == Cmd::PartNext ? 1 : ns_ - 1;
      if (pend_part_ >= 0) {  // переход ждёт границы: цель дальше или назад
        const int np = (pend_part_ + d) % ns_;
        pend_part_ = np == cur_part_ ? -1 : np;
        if (pend_part_ < 0) rec_after_switch_ = false;
        break;
      }
      const int target = (cur_part_ + d) % ns_;
      // из пустого парта и без квантования — сразу
      // сразу: текущий парт пустой, PART AT = сразу, или Free без «конца цикла» (тактов нет)
      if (!part_has_content(cur_part_) || set_.part_q == 0 || (!set_.quantize && set_.part_q != 3))
        switch_part(target);
      else
        pend_part_ = target;
      break;
    }
    case Cmd::Lock: t.locked = !t.locked; break;
  }
}

void Looper::rec_press(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  switch (t.st) {
    case TState::Empty:
      if (!set_.quantize) {
        start_rec(ti);
      } else if (!rolling_ && !has_content()) {
        // Транспорт стоит и лупов нет: запись сразу, такты считаем от неё.
        beats_ = 0.0;
        start_rec(ti);
      } else {
        t.st = TState::Armed;
      }
      break;
    case TState::Armed: t.st = TState::Empty; break;
    case TState::Rec: request_close(ti); break;
    case TState::Closing: break;
    case TState::Play:
      if (!t.stopped && !t.ustop) start_dub(ti);  // остановлена: молча игнорируем
      break;
    case TState::Dub:
      if (t.dub_end < 0) end_dub(ti);
      break;
  }
}

void Looper::start_rec(int ti) {
  Track& t = tr_[ti];
  if (!reserve(2, ti)) {
    t.st = TState::Empty;
    return;
  }
  int32_t b = pool_.alloc_buf();
  if (b < 0) {
    t.st = TState::Empty;
    return;
  }
  t.rec = b;
  t.rec_frames = 0;
  t.rec_target = 0;
  const int len_bars = set_.length[ti % ns_];  // длина задаётся по месту дорожки в парте
  if (set_.quantize && len_bars > 0) t.rec_target = (uint64_t)std::llround(len_bars * (double)bar_frames());
  // Quantize: запись начинается на черте такта, запоминаем её долю для подстройки фазы
  t.rec_q = set_.quantize;
  t.b0 = std::round(beats_ / bpb_) * bpb_;
  t.rec_bpf = bpf_;
  t.tspd = 1.0;
  t.st = TState::Rec;
  rec_peaks_start(ti);
}

// Волна записи основы. При фиксированной длине вся полоса — эта длина;
// при свободной колонка сначала 20 мс, а когда колонки кончаются, они сливаются попарно.
void Looper::rec_peaks_start(int ti) {
  Track& t = tr_[ti];
  t.rpk_step = t.rec_target ? (t.rec_target + kPeakCols - 1) / kPeakCols
                            : (uint64_t)std::llround(sr_ * kRecColS);
  if (t.rpk_step < 1) t.rpk_step = 1;
  t.rpk_n = 0;
  t.rpk_mn = t.rpk_mx = 0.0f;
  if (!peaks_ || !in_cur(ti)) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  clear_wave(pk.rec);
  pk.rec_col_s = (float)((double)t.rpk_step / sr_);
  __atomic_store_n(&pk.rec_cols, 0u, __ATOMIC_RELEASE);
  t.plive = true;
}

void Looper::rec_peaks(int ti, float l, float r) {
  Track& t = tr_[ti];
  if (l < t.rpk_mn) t.rpk_mn = l;
  if (r < t.rpk_mn) t.rpk_mn = r;
  if (l > t.rpk_mx) t.rpk_mx = l;
  if (r > t.rpk_mx) t.rpk_mx = r;
  if (++t.rpk_n < t.rpk_step) return;
  const float mn = t.rpk_mn, mx = t.rpk_mx;
  t.rpk_n = 0;
  t.rpk_mn = t.rpk_mx = 0.0f;
  if (!peaks_ || !in_cur(ti)) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  uint32_t n = pk.rec_cols;
  if (n >= kPeakCols) {  // колонки кончились: вдвое крупнее (512 сравнений раз в несколько секунд)
    for (uint32_t c = 0; c < kPeakCols / 2; ++c) {
      const int8_t a = pk.rec.mn[2 * c], b = pk.rec.mn[2 * c + 1];
      const int8_t x = pk.rec.mx[2 * c], y = pk.rec.mx[2 * c + 1];
      pk.rec.mn[c] = a < b ? a : b;
      pk.rec.mx[c] = x > y ? x : y;
    }
    for (uint32_t c = kPeakCols / 2; c < kPeakCols; ++c) pk.rec.mn[c] = pk.rec.mx[c] = 0;
    n = kPeakCols / 2;
    t.rpk_step *= 2;
    pk.rec_col_s = (float)((double)t.rpk_step / sr_);
  }
  pk.rec.mn[n] = peak_q(mn);
  pk.rec.mx[n] = peak_q(mx);
  __atomic_store_n(&pk.rec_cols, n + 1, __ATOMIC_RELEASE);
  t.plive = true;
}

void Looper::cancel_rec(int ti) {
  Track& t = tr_[ti];
  if (t.rec >= 0) pool_.free_buf(t.rec);
  t.rec = -1;
  t.rec_frames = t.rec_target = 0;
  t.st = TState::Empty;
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].rec_cols, 0u, __ATOMIC_RELEASE);
}

void Looper::request_close(int ti) {
  Track& t = tr_[ti];
  if (!set_.quantize) {
    close_loop(ti, t.rec_frames);
    return;
  }
  if (t.rec_target) return;  // фиксированная длина: закроется сама
  // Free-длина в Quantize: такт доигрывается до конца. Нажатие чуть позже черты
  // (нога опоздала, не больше kLatePressS) закрывает цикл на этой черте.
  double bf = (double)bar_frames();
  int64_t bars = (int64_t)std::floor((double)t.rec_frames / bf);
  const double late = (double)t.rec_frames - (double)bars * bf;
  if (bars < 1 || late > kLatePressS * sr_) ++bars;
  uint64_t L = (uint64_t)std::llround(bars * bf);
  if (L <= t.rec_frames) {
    close_loop(ti, L);
  } else {
    t.rec_target = L;
    t.st = TState::Closing;
  }
}

void Looper::close_loop(int ti, uint64_t L) {
  Track& t = tr_[ti];
  if (L < 2ull * xf_ + 2) return;  // слишком коротко: пишем дальше
  uint64_t d = t.rec_frames - L;   // уже записано после конца цикла (граница такта прошла)
  if (d >= L) d = L - 1;
  int32_t base = t.rec;
  t.rec = -1;
  t.rec_frames = t.rec_target = 0;
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].rec_cols, 0u, __ATOMIC_RELEASE);
  clear_redo(t);
  t.sum = base;
  t.len = L;
  push_undo(t, Entry{U_SNAP, -1});
  t.pos = 0.0;
  t.rate = 1.0;
  t.dir = 1;
  t.rate_step = 0.0;
  t.muted = t.mute_pending = false;
  t.gain = 1.0f;
  t.seam_t = 0;
  t.synced = t.rec_q;
  t.loop_beats = std::fmax(1.0, std::round((double)L * t.rec_bpf));
  t.st = TState::Play;
  if (set_.rop) start_dub(ti);
  // Хвост, записанный после конца цикла, ложится на начало: стык и (в ROP) овердаб.
  for (uint64_t k = 0; k < d; ++k) {
    float l, r;
    pool_.read(pool_.buf(base), L + k, l, r);
    if (t.seam_t < xf_) seam_write(t, l, r);
    if (t.st == TState::Dub && t.layer >= 0) dub_write(ti, k, l, r);
  }
  pool_.set_len(base, L);
  t.pos = (double)d;
  // Дорожка начинает звучать: короткий fade in, иначе вход хвоста на полной громкости щёлкнет.
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = -1;
  t.xf_owned = false;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  mark_peaks(t);
}

void Looper::start_dub(int ti) {
  Track& t = tr_[ti];
  if (t.busy || t.sum < 0) return;
  if (!reserve(2, ti)) return;
  int32_t b = pool_.alloc_buf();
  if (b < 0) return;
  pool_.set_len(b, t.len);
  clear_redo(t);
  t.layer = b;
  t.dub_t = 0;
  t.dub_end = -1;
  t.wfirst = true;
  t.undo_after_dub = t.reset_after_dub = false;
  t.st = TState::Dub;
  if (peaks_ && in_cur(ti)) {  // волна нового слоя растёт вживую
    PeakTrack& pk = peaks_->t[ti % ns_];
    clear_wave(pk.dub);
    __atomic_store_n(&pk.dub_active, 1u, __ATOMIC_RELEASE);
    t.plive = true;
  }
}

void Looper::end_dub(int ti) {
  Track& t = tr_[ti];
  if (t.st == TState::Dub && t.dub_end < 0) t.dub_end = 0;
}

void Looper::finish_dub(int ti) {
  Track& t = tr_[ti];
  if (t.layer >= 0) push_undo(t, Entry{U_DELTA, t.layer});
  t.layer = -1;
  t.dub_end = -1;
  t.st = TState::Play;
  mark_peaks(t);
  if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].dub_active, 0u, __ATOMIC_RELEASE);
  if (t.lock_to >= 0) {  // Lock-копия ждала конца овердаба, чтобы затухание попало и в неё;
    t.lock_ready = true;  // делаем её в начале следующего кадра (этот кадр уже прозвучал)
    clones_pending_ = true;
  }
  if (t.undo_after_dub) {
    t.undo_after_dub = false;
    undo(ti);
  } else if (t.reset_after_dub) {
    t.reset_after_dub = false;
    reset_track(ti);
  }
}

void Looper::undo(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  switch (t.st) {
    case TState::Armed: t.st = TState::Empty; return;
    case TState::Rec:
    case TState::Closing: cancel_rec(ti); return;
    case TState::Dub:
      end_dub(ti);
      t.undo_after_dub = true;
      return;
    default: break;
  }
  if (t.undo_n == 0) return;
  uint32_t top = (t.undo_lo + t.undo_n - 1) % kMaxUndo;
  Entry e = t.undo[top];
  if (e.type == U_SNAP) {
    --t.undo_n;
    int32_t old = t.sum;
    if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_SNAP, old};
    swap_to(ti, e.buf, false);
  } else {
    post_job(ti, 1, e.buf);
  }
}

void Looper::redo(int ti) {
  Track& t = tr_[ti];
  if (t.busy || (t.st != TState::Play && t.st != TState::Empty) || t.redo_n == 0) return;
  Entry e = t.redo[t.redo_n - 1];
  if (e.type == U_SNAP) {
    --t.redo_n;
    push_undo(t, Entry{U_SNAP, t.sum});
    swap_to(ti, e.buf, false);
  } else {
    post_job(ti, 2, e.buf);
  }
}

void Looper::reset_track(int ti) {
  Track& t = tr_[ti];
  if (t.busy) return;
  if (t.st == TState::Armed) {
    t.st = TState::Empty;
    return;
  }
  if (t.st == TState::Rec || t.st == TState::Closing) {
    cancel_rec(ti);
    return;
  }
  if (t.st == TState::Dub) {  // сначала плавно закрыть овердаб (слой сохранится для undo), потом reset
    end_dub(ti);
    t.reset_after_dub = true;
    return;
  }
  if (t.sum < 0) return;
  clear_redo(t);
  push_undo(t, Entry{U_SNAP, t.sum});
  swap_to(ti, -1, false);
}

// Смена сетки: если рабочий поток считает undo/redo, ждём его ответа (как удержание Reset).
void Looper::set_grid(int n) {
  if (!valid_grid(n) || (n == ns_ && grid_pending_ < 0)) return;
  grid_pending_ = n;
  if (!job_pending_ && !job_running_) apply_grid();
}

void Looper::apply_grid() {
  const int n = grid_pending_;
  grid_pending_ = -1;
  if (!valid_grid(n) || n == ns_) return;
  ns_ = n;
  reset_all_now(false);  // звучащие лупы затухают; дорожки за краем новой сетки тоже доигрывают затухание
}

void Looper::reset_all_now(bool full) {
  user_stop_ = false;
  ps_pending_ = 0;
  pend_part_ = -1;
  rec_after_switch_ = false;
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
    mark_peaks(t);
    if (peaks_ && in_cur(ti)) __atomic_store_n(&peaks_->t[ti % ns_].dub_active, 0u, __ATOMIC_RELEASE);
    if (t.rec >= 0) pool_.free_buf(t.rec);
    if (t.layer >= 0) pool_.free_buf(t.layer);
    t.rec = t.layer = -1;
    clear_redo(t);
    while (t.undo_n) {
      drop_entry_bufs(t, t.undo[t.undo_lo]);
      t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
      --t.undo_n;
    }
    t.undo_lo = 0;
    // звучащее затухает, потом уходит в пул
    if (t.sum >= 0) {
      if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
      t.xf_from = t.sum;
      t.xf_owned = true;
      t.xf_pos = 0;
      t.xpos = t.pos;
      t.xf_len = xf_;
    }
    t.sum = -1;
    t.len = 0;
    t.st = TState::Empty;
    t.muted = t.mute_pending = false;
    t.gain = 1.0f;
    t.dir = 1;
    t.rate = 1.0;
    t.rate_step = 0.0;
    t.rev_wait = -1;
    t.synced = false;
    t.stopped = t.ustop = false;
    t.sgain = 1.0f;
    t.locked = false;
    t.lock_to = -1;
    t.lock_ready = false;
    if (full) t.mspd = 1.0f;
    t.mcur = t.spd = t.tspd = 1.0;
    t.phase_reset = t.phase_wait = false;
    t.dub_end = -1;
    t.undo_after_dub = t.reset_after_dub = false;
    t.seam_t = kNoSeam;
  }
  default_locks();
  if (full) reset_mixer();
  // курсор — на первую дорожку первого парта
  cur_part_ = 0;
  active_ = 0;
}

void Looper::reset_mixer() {
  for (auto& m : mix_) m = Mix();
  master_db_ = dry_db_ = 0.0f;
  lim_on_ = true;
  ++mix_epoch_;
}

// Матрица дорожки: выход L = in L * LL + in R * RL, выход R = in L * LR + in R * RR.
void Looper::mix_matrix(const Mix& m, float out[4]) {
  const float g = db_gain(m.level_db);
  const float p = m.pan < -1.0f ? -1.0f : (m.pan > 1.0f ? 1.0f : m.pan);
  if (m.mode == 0) {  // стерео: баланс с постоянной мощностью, в центре как записано
    const float th = (p + 1.0f) * 0.7853982f;
    out[0] = g * std::cos(th) * 1.4142136f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = g * std::sin(th) * 1.4142136f;
  } else {  // моно L или моно R: играет один вход, панорама ставит его с постоянной мощностью
    const float th = (p + 1.0f) * 0.7853982f;
    const float a = g * std::cos(th), b = g * std::sin(th);
    const bool left = m.mode == 1;
    out[0] = left ? a : 0.0f;  // L -> левый выход
    out[1] = left ? 0.0f : a;  // R -> левый
    out[2] = left ? b : 0.0f;  // L -> правый
    out[3] = left ? 0.0f : b;  // R -> правый
  }
}

void Looper::start_reverse(Track& t, double n) {
  t.dir = -t.dir;
  t.rate_step = 2.0 / n * t.dir;
}

void Looper::mute_slot(int slot) {
  if (slot < 0 || slot >= ns_) return;
  const int k = idx(slot);
  Track& t = tr_[k];
  if (t.st != TState::Play && t.st != TState::Dub) return;
  if (!set_.quantize || set_.mute_q == 0) apply_mute(k);
  else t.mute_pending = !t.mute_pending;
}

void Looper::apply_mute(int ti) {
  Track& t = tr_[ti];
  t.muted = !t.muted;
  t.mute_pending = false;
}

void Looper::leave_track(int ti) {
  Track& t = tr_[ti];
  if (t.st == TState::Armed) t.st = TState::Empty;
  else if (t.st == TState::Rec) request_close(ti);
  else if (t.st == TState::Dub) end_dub(ti);
}

// ---------------------------------------------------------------- undo, redo, память

void Looper::push_undo(Track& t, Entry e) {
  if (t.undo_n == kMaxUndo) {  // самый старый слой вливается в основу
    drop_entry_bufs(t, t.undo[t.undo_lo]);
    t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
    --t.undo_n;
  }
  t.undo[(t.undo_lo + t.undo_n) % kMaxUndo] = e;
  ++t.undo_n;
}

void Looper::clear_redo(Track& t) {
  for (uint32_t i = 0; i < t.redo_n; ++i) drop_entry_bufs(t, t.redo[i]);
  t.redo_n = 0;
}

void Looper::free_buf_safe(Track& t, int32_t b) {
  if (b < 0) return;
  if (b == t.xf_from) t.xf_owned = true;  // ещё звучит в кроссфейде: освободится после него
  else pool_.free_buf(b);
}

void Looper::drop_entry_bufs(Track& t, const Entry& e) { free_buf_safe(t, e.buf); }

bool Looper::reserve(uint32_t need, int prefer) {
  while (pool_.free_chunks() < need + kReserve || pool_.free_bufs() < 4) {
    bool freed = false;
    for (int i = 0; i < kTracks && !freed; ++i) {  // сначала то, что можно только вернуть (redo)
      Track& t = tr_[i];
      if (!t.busy && t.redo_n) {
        clear_redo(t);
        freed = true;
      }
    }
    for (int k = 0; k < kTracks && !freed; ++k) {  // потом самые старые слои, своя дорожка первой
      int i = (prefer + k) % kTracks;
      Track& t = tr_[i];
      if (t.busy || t.undo_n == 0) continue;
      drop_entry_bufs(t, t.undo[t.undo_lo]);
      t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
      --t.undo_n;
      freed = true;
    }
    if (!freed) return false;
  }
  return true;
}

void Looper::swap_to(int ti, int32_t new_sum, bool free_old) {
  Track& t = tr_[ti];
  mark_peaks(t);
  int32_t old = t.sum;
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = old;
  t.xf_owned = free_old;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  t.sum = new_sum;
  if (new_sum < 0) {
    t.rev_wait = -1;
    t.synced = false;
    t.stopped = t.ustop = false;
    t.sgain = 1.0f;
    t.st = TState::Empty;
    t.len = 0;
    t.muted = t.mute_pending = false;
    t.gain = 1.0f;
    t.seam_t = kNoSeam;
    return;
  }
  t.len = pool_.buf(new_sum).len;
  if (t.synced && bpf_ > 0) t.loop_beats = std::fmax(1.0, std::round((double)t.len * t.rec_bpf));
  if (old < 0) {
    t.pos = 0.0;
    t.dir = 1;
    t.rate = 1.0;
    t.rate_step = 0.0;
  } else {
    t.pos = std::fmod(t.pos, (double)t.len);
  }
  t.st = TState::Play;
}

bool Looper::post_job(int ti, uint8_t op, int32_t layer) {
  Track& t = tr_[ti];
  if (job_pending_ || job_running_ || t.sum < 0) return false;
  // запись снимаем со стека заранее, чтобы reserve() её не освободил
  if (op == 1) --t.undo_n;
  else --t.redo_n;
  uint64_t L = t.len;
  uint32_t need = chunks_for(L) + 1;
  int32_t dst = -1;
  bool ok = reserve(need, ti) && (dst = pool_.alloc_buf()) >= 0 && pool_.set_len(dst, L);
  for (uint32_t ci = 0; ok && ci < chunks_for(L); ++ci) ok = pool_.ensure(dst, ci, false) != nullptr;
  if (!ok) {
    if (dst >= 0) pool_.free_buf(dst);
    if (op == 1) push_undo(t, Entry{U_DELTA, layer});
    else t.redo[t.redo_n++] = Entry{U_DELTA, layer};
    return false;
  }
  job_ = WorkMsg{};
  job_.op = op;
  job_.track = (uint8_t)ti;
  job_.src = t.sum;
  job_.layer = layer;
  job_.dst = dst;
  job_.len = L;
  job_pending_ = true;
  t.busy = true;
  return true;
}

bool Looper::take_work(WorkMsg& m) {
  if (job_pending_) {
    m = job_;
    job_pending_ = false;
    job_running_ = true;
    return true;
  }
  if (!peaks_) return false;
  // пики: сумма и последние слои овердабов (их номера с начала дорожки — для цвета)
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (!t.pdirty || t.pinflight || t.busy) continue;
    m = WorkMsg{};
    m.op = 3;
    m.track = (uint8_t)k;
    m.slot = (uint8_t)slot_of(k);
    m.src = t.sum;
    m.layer = m.dst = -1;
    m.len = t.sum >= 0 ? t.len : 0;
    m.gen = t.pgen;
    for (uint32_t j = t.undo_n; j > 0 && m.nl < kPeakLayers; --j) {
      const Entry& e = t.undo[(t.undo_lo + j - 1) % kMaxUndo];
      if (e.type != U_DELTA) break;  // ниже — основа или предыдущая запись
      m.lay[m.nl] = e.buf;
      m.lno[m.nl] = j - 1;
      ++m.nl;
    }
    t.pdirty = false;
    t.pinflight = true;
    return true;
  }
  return false;
}

void Looper::cancel_work(const WorkMsg& m) {
  if (m.op == 3) {
    Track& t = tr_[m.track];
    t.pinflight = false;
    t.pdirty = true;
    return;
  }
  if (!job_running_) return;
  job_running_ = false;
  Track& t = tr_[job_.track];
  t.busy = false;
  pool_.free_buf(job_.dst);
  if (job_.op == 1) push_undo(t, Entry{U_DELTA, job_.layer});
  else if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_DELTA, job_.layer};
  if (reset_all_pending_) {
    reset_all_pending_ = false;
    reset_all_now();
  }
  if (grid_pending_ > 0) apply_grid();
}

void Looper::do_work(const WorkMsg& m, WorkResp& r) {
  r.op = m.op;
  r.track = m.track;
  r.gen = m.gen;
  if (m.op == 3) {
    if (!peaks_) return;
    PeakTrack& pk = peaks_->t[m.slot];
    seq_begin(pk);
    compute_wave(pool_, m.src, m.len, pk.sum);
    for (uint32_t i = 0; i < m.nl; ++i) {
      compute_wave(pool_, m.lay[i], m.len, pk.layer[i]);
      pk.layer_no[i] = m.lno[i];
    }
    pk.nlayers = m.nl;
    seq_end(pk);
    return;
  }
  const Buf& src = pool_.buf(m.src);
  const Buf& lay = pool_.buf(m.layer);
  const Buf& dst = pool_.buf(m.dst);
  const float sign = m.op == 1 ? -1.0f : 1.0f;
  const uint32_t nc = chunks_for(m.len);
  for (uint32_t ci = 0; ci < nc; ++ci) {
    Chunk* d = pool_.chunk(pool_.entry(dst, ci));
    const Chunk* s = pool_.chunk(pool_.entry(src, ci));
    uint32_t lid = pool_.entry(lay, ci);
    if (lid == kNone) {
      for (uint32_t i = 0; i < kChunkFrames; ++i) {
        d->l[i] = s->l[i];
        d->r[i] = s->r[i];
      }
    } else {
      const Chunk* x = pool_.chunk(lid);
      for (uint32_t i = 0; i < kChunkFrames; ++i) {
        d->l[i] = s->l[i] + sign * x->l[i];
        d->r[i] = s->r[i] + sign * x->r[i];
      }
    }
  }
}

void Looper::work_done(const WorkResp& r) {
  if (r.op == 3) {  // пики готовы; если дорожка успела измениться — посчитать ещё раз
    Track& t = tr_[r.track];
    t.pinflight = false;
    if (r.gen != t.pgen) t.pdirty = true;
    return;
  }
  job_running_ = false;
  Track& t = tr_[r.track];
  t.busy = false;
  mark_peaks(t);
  // новая сумма готова: кроссфейд со старой, старая уходит в пул
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = t.sum;
  t.xf_owned = true;
  t.xf_pos = 0;
  t.xpos = t.pos;
  t.xf_len = xf_;
  t.sum = job_.dst;
  if (r.op == 1) {
    if (t.redo_n < kMaxUndo) t.redo[t.redo_n++] = Entry{U_DELTA, job_.layer};
    else pool_.free_buf(job_.layer);
  } else {
    push_undo(t, Entry{U_DELTA, job_.layer});
  }
  if (reset_all_pending_) {
    reset_all_pending_ = false;
    reset_all_now();
  }
  if (grid_pending_ > 0) apply_grid();
}

// ---------------------------------------------------------------- звук

bool Looper::has_content() const {
  for (const auto& t : tr_)
    if (t.sum >= 0 || t.st == TState::Rec || t.st == TState::Closing) return true;
  return false;
}

uint64_t Looper::bar_frames() const {
  double bpf = bpf_ > 1e-9 ? bpf_ : 2.0 / sr_;
  return (uint64_t)std::llround(bpb_ / bpf);
}

void Looper::read_sum(const Track& t, int32_t b, double pos, float& l, float& r) const {
  (void)t;
  if (b < 0) {
    l = r = 0.0f;
    return;
  }
  const Buf& x = pool_.buf(b);
  const uint64_t L = x.len;
  if (L == 0) {
    l = r = 0.0f;
    return;
  }
  const double fl = std::floor(pos);
  if (fl == pos) {  // ровно 1x или -1x: прямое чтение
    pool_.read(x, wrap_index((int64_t)fl, L), l, r);
    return;
  }
  // кубическая интерполяция Эрмита (Catmull-Rom)
  const int64_t i = (int64_t)fl;
  const float f = (float)(pos - fl);
  float l0, r0, l1, r1, l2, r2, l3, r3;
  pool_.read(x, wrap_index(i - 1, L), l0, r0);
  pool_.read(x, wrap_index(i, L), l1, r1);
  pool_.read(x, wrap_index(i + 1, L), l2, r2);
  pool_.read(x, wrap_index(i + 2, L), l3, r3);
  auto cr = [f](float y0, float y1, float y2, float y3) {
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
  };
  l = cr(l0, l1, l2, l3);
  r = cr(r0, r1, r2, r3);
}

void Looper::write_add(int32_t b, uint64_t p, float l, float r) {
  Chunk* c = pool_.writable(b, (uint32_t)(p >> kChunkShift), true);
  if (!c) return;
  c->l[p & kChunkMask] += l;
  c->r[p & kChunkMask] += r;
}

// Хвост после закрытия цикла: затухает поверх начала цикла (стык без щелчка).
void Looper::seam_write(Track& t, float l, float r) {
  const uint64_t k = t.seam_t++;
  if (t.sum < 0 || t.len == 0) return;
  const float fo = 1.0f - (float)k / (float)xf_;
  write_add(t.sum, k % t.len, l * fo, r * fo);
}

// Огибающая овердаба на этот кадр входа (вход и выход по 10 мс).
float Looper::dub_env(Track& t) {
  float env = t.dub_t < xf_ ? (float)t.dub_t / (float)xf_ : 1.0f;
  if (t.dub_end >= 0) {
    float e2 = 1.0f - (float)t.dub_end / (float)xf_;
    if (e2 < env) env = e2;
  }
  ++t.dub_t;
  return env;
}

// Прибавить к слою и сумме в позиции p. false — кончилась память.
bool Looper::dub_put(int ti, uint64_t p, float l, float r) {
  Track& t = tr_[ti];
  Chunk* c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c && reserve(1, ti)) c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c) return false;
  c->l[p & kChunkMask] += l;
  c->r[p & kChunkMask] += r;
  write_add(t.sum, p, l, r);
  if (peaks_ && in_cur(ti)) live_peaks(ti, p);
  return true;
}

void Looper::dub_write(int ti, uint64_t p, float l, float r) {
  Track& t = tr_[ti];
  const float env = dub_env(t);
  if (!dub_put(ti, p, l * env, r * env)) end_dub(ti);  // память кончилась: овердаб закрывается
  t.wprev = (double)p;
  t.wl_prev = l;
  t.wr_prev = r;
  t.wfirst = false;
  if (t.dub_end >= 0 && ++t.dub_end >= (int64_t)xf_) finish_dub(ti);
}

// Овердаб на скорости не 1× (как на ленте): головка прошла от прошлой позиции до текущей,
// в каждую пройденную позицию лупа пишем вход, интерполированный между прошлым и текущим кадром.
void Looper::dub_resampled(int ti, float il, float ir, double step) {
  Track& t = tr_[ti];
  const double w1 = t.pos;
  if (step == 1.0 && w1 == std::floor(w1)) {  // ровно 1×: как раньше, кадр в кадр
    dub_write(ti, wrap_index((int64_t)w1, t.len), il, ir);
    return;
  }
  const double L = (double)t.len;
  double d = t.wfirst ? step : w1 - t.wprev;  // сколько прошла головка за кадр
  if (d > L / 2) d -= L;
  else if (d < -L / 2) d += L;
  const double w0 = w1 - d;
  const float env = dub_env(t);
  bool ok = true;
  if (d > 0) {
    for (int64_t n = (int64_t)std::floor(w0) + 1; n <= (int64_t)std::floor(w1) && ok; ++n) {
      const float a = (float)(((double)n - w0) / d);
      ok = dub_put(ti, wrap_index(n, t.len), (t.wl_prev + (il - t.wl_prev) * a) * env,
                   (t.wr_prev + (ir - t.wr_prev) * a) * env);
    }
  } else if (d < 0) {
    for (int64_t n = (int64_t)std::ceil(w1); n < (int64_t)std::ceil(w0) && ok; ++n) {
      const float a = (float)((w0 - (double)n) / -d);
      ok = dub_put(ti, wrap_index(n, t.len), (t.wl_prev + (il - t.wl_prev) * a) * env,
                   (t.wr_prev + (ir - t.wr_prev) * a) * env);
    }
  }
  if (!ok) end_dub(ti);
  t.wprev = w1;
  t.wl_prev = il;
  t.wr_prev = ir;
  t.wfirst = false;
  if (t.dub_end >= 0 && ++t.dub_end >= (int64_t)xf_) finish_dub(ti);
}

// Аудиопоток дорисовывает колонку под позицией: волна суммы и текущего слоя растёт вживую.
void Looper::live_peaks(int ti, uint64_t p) {
  Track& t = tr_[ti];
  if (!t.len) return;
  PeakTrack& pk = peaks_->t[ti % ns_];
  const uint32_t c = (uint32_t)(p * kPeakCols / t.len);
  float l, r;
  pool_.read(pool_.buf(t.sum), p, l, r);
  int8_t lo = peak_q(l < r ? l : r), hi = peak_q(l > r ? l : r);
  if (lo < pk.sum.mn[c]) pk.sum.mn[c] = lo;
  if (hi > pk.sum.mx[c]) pk.sum.mx[c] = hi;
  pool_.read(pool_.buf(t.layer), p, l, r);
  lo = peak_q(l < r ? l : r);
  hi = peak_q(l > r ? l : r);
  if (lo < pk.dub.mn[c]) pk.dub.mn[c] = lo;
  if (hi > pk.dub.mx[c]) pk.dub.mx[c] = hi;
  t.plive = true;
}

void Looper::on_grid(bool beat, bool bar) {
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
    if (t.st == TState::Armed && bar) start_rec(ti);
    if (t.mute_pending && ((set_.mute_q == 1 && beat) || (set_.mute_q == 2 && bar))) apply_mute(ti);
    if (t.phase_wait && ((set_.mute_q == 1 && beat) || (set_.mute_q >= 2 && bar))) {
      t.phase_wait = false;
      resync(ti);
    }
  }
  if (ps_pending_ && ((set_.mute_q == 1 && beat) || (set_.mute_q >= 2 && bar))) {
    apply_playstop(ps_pending_ == 1);
    ps_pending_ = 0;
  }
  if (pend_part_ >= 0 && ((set_.part_q == 1 && beat) || (set_.part_q == 2 && bar))) switch_part(pend_part_);
}

void Looper::track_sample(int ti, uint32_t i, float il, float ir, float& ol, float& orr) {
  Track& t = tr_[ti];
  // Дорожка ушедшего парта: затихает по fade out, потом не обрабатывается.
  // Lock-копия ждёт конца овердаба — до тех пор старая дорожка звучит.
  const bool away = !in_cur(ti) && t.lock_to < 0;
  if (away && t.sgain <= 0.0f && t.xf_len == 0 && t.st != TState::Rec && t.st != TState::Closing &&
      t.st != TState::Dub)
    return;

  if (t.st == TState::Rec || t.st == TState::Closing) {
    const uint64_t f = t.rec_frames;
    Chunk* c = pool_.ensure(t.rec, (uint32_t)(f >> kChunkShift), false);
    if (!c && reserve(1, ti)) c = pool_.ensure(t.rec, (uint32_t)(f >> kChunkShift), false);
    if (!c) {  // память кончилась: закрываем как есть
      if (f >= 2ull * xf_ + 2) close_loop(ti, f);
      else cancel_rec(ti);
    } else {
      const float g = f < xf_ ? (float)f / (float)xf_ : 1.0f;  // fade in начала записи
      c->l[f & kChunkMask] = il * g;
      c->r[f & kChunkMask] = ir * g;
      rec_peaks(ti, il * g, ir * g);
      ++t.rec_frames;
      if (t.rec_target && t.rec_frames >= t.rec_target) {
        close_loop(ti, t.rec_target);
        return;  // этот кадр уже последний в цикле; следующий вход — первый кадр хвоста
      }
    }
  }

  const bool playing = t.st == TState::Play || t.st == TState::Dub;
  const bool fading = t.xf_len > 0;
  if (!playing && !fading) return;

  // чтение (с кроссфейдом со старой суммы)
  float l = 0.0f, r = 0.0f;
  if (playing) read_sum(t, t.sum, t.pos, l, r);
  if (fading) {
    float fl, fr;
    read_sum(t, t.xf_from, t.xpos, fl, fr);
    const float g = (float)t.xf_pos / (float)t.xf_len;
    l = fl * (1.0f - g) + l * g;
    r = fr * (1.0f - g) + r * g;
    if (++t.xf_pos >= t.xf_len) {
      if (t.xf_owned && t.xf_from >= 0) pool_.free_buf(t.xf_from);
      t.xf_from = -1;
      t.xf_owned = false;
      t.xf_len = t.xf_pos = 0;
    }
  }
  // mute: плавно по fade out / fade in
  const float target = t.muted ? 0.0f : 1.0f;
  if (t.gain != target) {
    const float ms = t.muted ? set_.fade_out_ms : set_.fade_in_ms;
    const float step = 1000.0f / ((ms > 1.0f ? ms : 1.0f) * (float)sr_);
    t.gain = t.gain < target ? std::fmin(target, t.gain + step) : std::fmax(target, t.gain - step);
  }
  // остановка по транспорту: затухает по fade out, входит обратно по fade in
  const float starget = (t.stopped || t.ustop || away) ? 0.0f : 1.0f;
  if (t.sgain != starget) {
    const float ms = starget == 0.0f ? set_.fade_out_ms : set_.fade_in_ms;
    const float step = 1000.0f / ((ms > 1.0f ? ms : 1.0f) * (float)sr_);
    t.sgain = t.sgain < starget ? std::fmin(starget, t.sgain + step) : std::fmax(starget, t.sgain - step);
  }
  const float g = t.gain * t.sgain;
  const float* c = mcur_[ti];
  const float* d = mstep_[ti];
  const float fi = (float)i;
  const float yl = (l * (c[0] + d[0] * fi) + r * (c[1] + d[1] * fi)) * g;
  const float yr = (l * (c[2] + d[2] * fi) + r * (c[3] + d[3] * fi)) * g;
  ol += yl;
  orr += yr;
  const float a = std::fmax(std::fabs(yl), std::fabs(yr));
  if (a > bmax_[ti]) bmax_[ti] = a;

  // шаг головки за кадр: направление (разворот reverse) × скорость
  const double step = t.rate * t.spd;
  // запись: хвост на стыке и овердаб
  if (playing) {
    if (t.seam_t < xf_) seam_write(t, il, ir);
    if (t.st == TState::Dub && t.layer >= 0) dub_resampled(ti, il, ir, step);
  }

  // движение: reverse разворачивается как лента (скорость плавно проходит через 0)
  uint64_t L = t.len;
  if (L == 0 && t.xf_from >= 0) L = pool_.buf(t.xf_from).len;
  if (L == 0) return;
  if (t.rev_wait >= 0 && t.rev_wait-- == 0) start_reverse(t, t.rev_n);
  if (t.rate_step != 0.0) {
    t.rate += t.rate_step;
    if ((t.rate_step > 0 && t.rate >= 1.0) || (t.rate_step < 0 && t.rate <= -1.0)) {
      t.rate = t.rate_step > 0 ? 1.0 : -1.0;
      t.rate_step = 0.0;
      if (t.spd == 1.0) t.pos = std::round(t.pos);  // на 1× дальше снова прямое чтение
    }
  }
  if (t.xf_len > 0 && t.xf_from >= 0) {
    const double XL = (double)pool_.buf(t.xf_from).len;
    t.xpos += step;
    if (XL > 0) {
      if (t.xpos >= XL) t.xpos -= XL;
      else if (t.xpos < 0.0) t.xpos += XL;
    }
  }
  t.pos += step;
  bool wrapped = false;
  if (t.pos >= (double)L) {
    t.pos -= (double)L;
    wrapped = true;
  } else if (t.pos < 0.0) {
    t.pos += (double)L;
    wrapped = true;
  }
  if (wrapped && t.mute_pending && set_.mute_q == 3) apply_mute(ti);
  if (wrapped && ti == master_ && pend_part_ >= 0 && set_.part_q == 3) part_due_ = true;
}

void Looper::set_time(const TimeInfo& ti) {
  bpf_ = ti.beats_per_frame;
  bpb_ = ti.bpb > 0 ? ti.bpb : 4;
  const bool started = ti.rolling && !rolling_;
  const bool jumped = ti.rolling && rolling_ && std::fabs(ti.beats - beats_) > 0.05;
  const bool stopped_now = !ti.rolling && rolling_;
  rolling_ = ti.rolling;
  if (rolling_) beats_ = ti.beats;  // сетка Zynthian; иначе продолжаем свой счёт
  // Транспорт пошёл или перескочил: лупы в Quantize встают в фазу с позицией Zynthian.
  if (time_seen_ && pool_.ok()) {
    if (started) transport_start();
    else if (jumped)
      for (int k = idx(0); k < idx(0) + ns_; ++k) resync(k);
    else if (stopped_now) transport_stop();
  }
  time_seen_ = true;
}

// Транспорт Zynthian остановлен: играющие дорожки плавно затихают (овердаб закрывается).
// Запись основы не прерывается: лупер считает такты сам.
void Looper::transport_stop() {
  tstop_ = true;
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (t.st != TState::Play && t.st != TState::Dub) continue;
    if (t.st == TState::Dub) end_dub(k);
    t.rev_wait = -1;
    t.stopped = true;
  }
}

// Транспорт пошёл: остановленные дорожки входят обратно в фазе с Zynthian
// (записанные в Free — с начала цикла), остальные встают в фазу.
void Looper::transport_start() {
  tstop_ = false;
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (!t.stopped) {
      if (!t.ustop) resync(k);
      continue;
    }
    t.stopped = false;
    if (!t.ustop) wake(k);  // остановленные педалью Play/Stop ждут педали
  }
}

// Дорожка снова звучит: в фазе с сеткой (записанные в Free — с начала цикла).
void Looper::wake(int ti) {
  Track& t = tr_[ti];
  if (t.sgain > 0.001f) {  // не успела затихнуть: перескок через кроссфейд
    resync(ti);
    return;
  }
  // тишина: просто ставим позицию, огибающая сама нарастёт
  if (t.synced && t.len && t.loop_beats > 0 && t.dir > 0 && t.rate_step == 0.0) {
    double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
    if (ph < 0) ph += 1.0;
    t.pos = std::round(ph * (double)t.len);
    if (t.pos >= (double)t.len) t.pos = 0.0;
  } else {
    t.pos = t.dir > 0 ? 0.0 : (double)(t.len ? t.len - 1 : 0);
  }
}

// Play/Stop: стоп глушит играющие дорожки (овердаб закрывается), старт запускает их
// с начала такта в фазе с сеткой. Действует только на лупер, транспорт Zynthian не трогает.
// Явный старт запускает и дорожки, остановленные транспортом.
void Looper::apply_playstop(bool stop) {
  user_stop_ = stop;
  if (!stop) tstop_ = false;
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    Track& t = tr_[k];
    if (t.st != TState::Play && t.st != TState::Dub) continue;
    if (stop) {
      if (t.st == TState::Dub) end_dub(k);
      t.rev_wait = -1;
      t.ustop = true;
    } else if (t.ustop || t.stopped) {
      t.ustop = t.stopped = false;
      wake(k);
    }
  }
}

// ---------------------------------------------------------------- скорость и следование темпу

float Looper::snap_speed(float s) const {
  if (set_.spd_snap == 0) return s;
  const double st = 12.0 * std::log2((double)s);
  const double q = set_.spd_snap == 2 ? 12.0 : 1.0;  // октавы или полутоны
  return (float)std::exp2(std::round(st / q) * q / 12.0);
}

void Looper::speed_reset(int k) {
  Track& t = tr_[k];
  t.mspd = 1.0f;
  t.phase_reset = true;  // доедет до 1× — встанет в фазу с тактом
}

// Раз в блок: плавный ход ручной скорости и следование темпу, итоговая скорость дорожек.
// Следование темпу (Quantize) — как у ленты: скорость = темп сейчас / темп при записи.
// Пока дорожка может стоять в фазе с Zynthian (ручная 1×, вперёд, играет), скорость и фаза
// ведутся одним критически задемпфированным звеном второго порядка: при смене темпа скорость
// плавно (без излома) идёт к новой, чуть переходит её (≈13 % от шага темпа) ровно настолько,
// чтобы отставание по фазе ушло само. Раньше скорость шла за фильтром темпа, а отставание
// догонялось отдельной подстройкой ±2 % — высота уходила выше цели почти на секунду («плывёт»).
void Looper::update_speeds(uint32_t n) {
  const double glide = std::fmax(1.0, (double)set_.tempo_glide_ms) / 1000.0 * sr_;  // кадров
  const double w = 4.0 / glide;                                 // собственная частота, 1/кадр
  const double kt = 1.0 - std::exp(-(double)n * w);             // без фазы: первый порядок, τ = glide/4
  const double emax = 0.01 * sr_;                               // ошибка фазы в расчёте не больше 10 мс
  const double sg = (double)set_.spd_glide_ms / 1000.0 / 4.0 * sr_;
  const double ks = sg < 1.0 ? 1.0 : 1.0 - std::exp(-(double)n / sg);
  for (int k = 0; k < kTracks; ++k) {
    Track& t = tr_[k];
    // ручная: плавный ход в логарифмах (равномерно по высоте)
    const double target = (double)snap_speed(t.mspd);
    if (t.mcur != target) {
      double lc = std::log2(t.mcur), lt = std::log2(target);
      lc += (lt - lc) * ks;
      t.mcur = std::fabs(lt - lc) < 1e-5 ? target : std::exp2(lc);
    }
    const bool follow = t.rec_q && set_.quantize && t.rec_bpf > 0.0 && bpf_ > 0.0;
    const bool can_lock = follow && t.synced && t.len && t.loop_beats > 0 && t.dir > 0 && t.rate_step == 0.0 &&
                          t.mcur == 1.0 && (t.st == TState::Play || t.st == TState::Dub) &&
                          in_cur(k) && !t.stopped && !t.ustop && t.xf_len == 0;
    if (!follow) {
      t.tspd = 1.0;
    } else if (!can_lock) {
      t.tspd += (bpf_ / t.rec_bpf - t.tspd) * kt;
    } else {
      const double L = (double)t.len;
      double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
      if (ph < 0) ph += 1.0;
      double err = ph * L - t.pos;  // > 0: дорожка отстаёт
      if (err > L / 2) err -= L;
      if (err < -L / 2) err += L;
      if (std::fabs(err) > 0.05 * sr_) {
        resync(k);  // далеко (больше 50 мс): перескок через кроссфейд
        t.tspd = bpf_ / t.rec_bpf;
      } else {
        const double r = bpf_ * L / t.loop_beats;  // скорость сетки в кадрах дорожки за кадр
        // шагами не длиннее 0,1/w: устойчиво при любом размере блока
        const int m = 1 + (int)((double)n * w / 0.1);
        const double dt = (double)n / m;
        for (int j = 0; j < m; ++j) {
          const double e = err > emax ? emax : (err < -emax ? -emax : err);
          t.tspd += (w * w * e + 2.0 * w * (r - t.tspd)) * dt;
          err += (r - t.tspd) * dt;
        }
      }
    }
    const double sp = t.mcur * t.tspd;
    t.spd = sp;
    // после сброса скорости — в фазу с тактом
    if (t.phase_reset && t.mcur == 1.0) {
      t.phase_reset = false;
      if (t.synced) {
        if (!set_.quantize || set_.mute_q == 0) resync(k);
        else t.phase_wait = true;
      }
    }
  }
}

// ---------------------------------------------------------------- парты и Lock

bool Looper::part_has_content(int p) const {
  for (int k = p * ns_; k < (p + 1) * ns_; ++k) {
    const Track& t = tr_[k];
    if (t.sum >= 0 || t.st == TState::Rec || t.st == TState::Closing) return true;
  }
  return false;
}

uint32_t Looper::parts_mask() const {
  uint32_t m = 0;
  for (int p = 0; p < ns_; ++p)
    if (part_has_content(p)) m |= 1u << p;
  return m;
}

// По умолчанию первая дорожка каждого парта с Lock (решение пользователя).
void Looper::default_locks() {
  for (int p = 0; p < ns_; ++p) tr_[p * ns_].locked = true;
}

// Дорожка становится пустой и без истории (под Lock-копию в пустом парте).
void Looper::clear_track(int k) {
  Track& t = tr_[k];
  if (t.rec >= 0) pool_.free_buf(t.rec);
  if (t.layer >= 0) pool_.free_buf(t.layer);
  clear_redo(t);
  while (t.undo_n) {
    drop_entry_bufs(t, t.undo[t.undo_lo]);
    t.undo_lo = (t.undo_lo + 1) % kMaxUndo;
    --t.undo_n;
  }
  if (t.sum >= 0) free_buf_safe(t, t.sum);
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  const uint32_t pgen = t.pgen;
  const bool inflight = t.pinflight;
  t = Track();
  t.seam_t = kNoSeam;
  t.locked = k % ns_ == 0;
  t.pgen = pgen + 1;
  t.pdirty = true;
  t.pinflight = inflight;
}

// Lock-копия: новая дорожка делит куски памяти с оригиналом (copy-on-write) и продолжает
// с той же позиции в тот же сэмпл, а оригинал замолкает — шва нет. История копии с нуля.
void Looper::clone_into(int from, int to) {
  Track& o = tr_[from];
  if (o.sum < 0 || tr_[to].busy) return;
  clear_track(to);
  const int32_t b = pool_.clone_buf(o.sum);
  if (b < 0) return;
  Track& n = tr_[to];
  n.st = TState::Play;
  n.sum = b;
  n.len = o.len;
  n.pos = o.pos;
  n.rate = o.rate;
  n.dir = o.dir;
  n.rate_step = o.rate_step;
  n.rev_wait = o.rev_wait;
  n.rev_n = o.rev_n;
  n.synced = o.synced;
  n.rec_q = o.rec_q;
  n.b0 = o.b0;
  n.loop_beats = o.loop_beats;
  n.rec_bpf = o.rec_bpf;
  n.muted = o.muted;
  n.mute_pending = o.mute_pending;
  n.gain = o.gain;
  n.stopped = o.stopped;
  n.ustop = o.ustop;
  n.sgain = o.sgain;
  n.locked = true;
  n.mspd = o.mspd;
  n.mcur = o.mcur;
  n.spd = o.spd;
  n.tspd = o.tspd;
  mix_[to] = mix_[from];
  for (int j = 0; j < 4; ++j) mcur_[to][j] = mcur_[from][j];
  ++mix_epoch_;
  o.sgain = 0.0f;  // звук продолжает копия
  o.rev_wait = -1;
  o.mute_pending = false;
}

// Переход в парт p. Пустой парт: дорожки с Lock продолжают там же в той же фазе,
// остальные затихают, активной становится первая свободная. Записанный парт:
// его лупы начинают с начала цикла, старый парт затихает. Запись и овердаб закрываются.
void Looper::switch_part(int p) {
  if (p == cur_part_ || p < 0 || p >= ns_) return;
  const int old = cur_part_;
  pend_part_ = -1;
  for (int i = 0; i < ns_; ++i) {
    const int k = old * ns_ + i;
    Track& t = tr_[k];
    if (t.st == TState::Armed) t.st = TState::Empty;
    else if (t.st == TState::Rec) request_close(k);
    else if (t.st == TState::Dub) end_dub(k);
    t.mute_pending = false;
  }
  const bool empty_target = !part_has_content(p);
  cur_part_ = p;
  if (empty_target) {
    int first_free = -1;
    for (int i = 0; i < ns_; ++i) {
      const int o = old * ns_ + i, n = p * ns_ + i;
      Track& ot = tr_[o];
      if (ot.locked && ot.sum >= 0 && (ot.st == TState::Play || ot.st == TState::Dub)) {
        if (ot.st == TState::Dub) ot.lock_to = n;  // копия после затухания овердаба
        else clone_into(o, n);
      } else if (first_free < 0) {
        first_free = i;
      }
    }
    active_ = first_free >= 0 ? first_free : 0;
  } else {
    for (int i = 0; i < ns_; ++i) {
      const int k = p * ns_ + i;
      Track& t = tr_[k];
      if (t.sum < 0 || (t.st != TState::Play && t.st != TState::Dub)) continue;
      t.pos = t.dir > 0 ? 0.0 : (double)(t.len - 1);
      t.rate = (double)t.dir;
      t.rate_step = 0.0;
      t.rev_wait = -1;
      t.mute_pending = false;
      t.stopped = tstop_;
      t.ustop = user_stop_;
      t.sgain = (t.stopped || t.ustop) ? 0.0f : 1.0f;
      if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
      t.xf_from = -1;  // короткий fade in от начала цикла
      t.xf_owned = false;
      t.xf_pos = 0;
      t.xf_len = xf_;
      t.xpos = t.pos;
    }
  }
  for (int k = p * ns_; k < (p + 1) * ns_; ++k) mark_peaks(tr_[k]);
  if (peaks_)
    for (int i = 0; i < ns_; ++i) __atomic_store_n(&peaks_->t[i].dub_active, 0u, __ATOMIC_RELEASE);
  if (rec_after_switch_) {  // запись с границы перехода на активной дорожке нового парта
    rec_after_switch_ = false;
    const int k = idx(active_);
    Track& t = tr_[k];
    if (t.st == TState::Empty) start_rec(k);
    else if (t.st == TState::Play && !t.stopped && !t.ustop) start_dub(k);
  }
}

void Looper::resync(int ti) {
  Track& t = tr_[ti];
  if (!t.synced || t.len == 0 || t.loop_beats <= 0 || t.dir < 0 || t.rate_step != 0.0) return;
  if (t.st != TState::Play && t.st != TState::Dub) return;
  double ph = std::fmod((beats_ - t.b0) / t.loop_beats, 1.0);
  if (ph < 0) ph += 1.0;
  const double L = (double)t.len;
  const double want = std::round(ph * L);
  double diff = std::fabs(want - t.pos);
  if (diff > L / 2) diff = L - diff;
  if (diff < sr_ * 0.001) return;  // и так в фазе (меньше 1 мс)
  // перескок через кроссфейд: старая позиция затухает, новая нарастает
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = t.sum;
  t.xf_owned = false;
  t.xf_pos = 0;
  t.xf_len = xf_;
  t.xpos = t.pos;
  t.pos = want >= L ? 0.0 : want;
}

void Looper::process(const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n,
                     const TimeInfo& ti) {
  set_time(ti);
  // микшер: к целевым значениям плавно (~20 мс), внутри блока — линейно
  const float ks = n ? 1.0f - (float)std::exp(-(double)n / (kMixSmoothS * sr_)) : 0.0f;
  for (int k = 0; k < kTracks; ++k) {
    float tgt[4];
    mix_matrix(mix_[k], tgt);
    for (int j = 0; j < 4; ++j) {
      const float end = mcur_[k][j] + (tgt[j] - mcur_[k][j]) * ks;
      mstep_[k][j] = n ? (end - mcur_[k][j]) / (float)n : 0.0f;
    }
    bmax_[k] = 0.0f;
  }
  const float mg0 = master_g_, dg0 = dry_g_;
  master_g_ += (db_gain(master_db_) - master_g_) * ks;
  dry_g_ += (db_gain(dry_db_) - dry_g_) * ks;
  const float mgs = n ? (master_g_ - mg0) / (float)n : 0.0f;
  const float dgs = n ? (dry_g_ - dg0) / (float)n : 0.0f;
  const float lim_rel = (float)std::exp(-1.0 / (kLimRelS * sr_));
  update_speeds(n);
  float mbl = 0.0f, mbr = 0.0f;
  // «конец цикла» для смены парта — по самой длинной играющей дорожке текущего парта
  master_ = -1;
  uint64_t longest = 0;
  for (int k = idx(0); k < idx(0) + ns_; ++k) {
    const Track& t = tr_[k];
    if ((t.st == TState::Play || t.st == TState::Dub) && t.len > longest) {
      longest = t.len;
      master_ = k;
    }
  }
  if (pend_part_ >= 0 && set_.part_q == 3 && master_ < 0) part_due_ = true;  // нечему доигрывать
  for (uint32_t i = 0; i < n; ++i) {
    // beats_ — доля этого кадра; граница там, где доля перешла через целое с прошлого кадра
    if (set_.quantize) {
      const int64_t a = (int64_t)std::floor(beats_ - bpf_), c = (int64_t)std::floor(beats_);
      if (a != c) on_grid(true, ((c % bpb_) + bpb_) % bpb_ == 0);
    } else {
      for (int k = 0; k < kTracks; ++k)
        if (tr_[k].st == TState::Armed) start_rec(k);
    }
    if (part_due_) {  // самый длинный луп дошёл до конца цикла: переход
      part_due_ = false;
      if (pend_part_ >= 0) switch_part(pend_part_);
    }
    if (clones_pending_) {
      clones_pending_ = false;
      for (int k = 0; k < kTracks; ++k)
        if (tr_[k].lock_ready) {
          const int to = tr_[k].lock_to;
          tr_[k].lock_ready = false;
          tr_[k].lock_to = -1;
          clone_into(k, to);
        }
    }
    const float il = in_l ? in_l[i] : 0.0f;
    const float ir = in_r ? in_r[i] : 0.0f;
    float ol = 0.0f, orr = 0.0f;
    if (pool_.ok())
      for (int k = 0; k < kTracks; ++k) {
        const Track& t = tr_[k];  // пустая без затухания: делать нечего (дорожек до 36)
        if ((t.st == TState::Empty || t.st == TState::Armed) && t.xf_len == 0) continue;
        track_sample(k, i, il, ir, ol, orr);
      }
    // мастер лупов: уровень, мягкий ограничитель, пики; сухой сигнал мимо ограничителя
    const float mg = mg0 + mgs * (float)i;
    ol *= mg;
    orr *= mg;
    if (lim_on_) {
      const float x = std::fmax(std::fabs(ol), std::fabs(orr));
      lim_env_ = x > lim_env_ ? x : x + (lim_env_ - x) * lim_rel;
      if (lim_env_ > kLimThr) {
        const float gr = kLimThr / lim_env_;
        ol *= gr;
        orr *= gr;
      }
      ol = soft_clip(ol);
      orr = soft_clip(orr);
    }
    if (std::fabs(ol) > mbl) mbl = std::fabs(ol);
    if (std::fabs(orr) > mbr) mbr = std::fabs(orr);
    const float dg = dg0 + dgs * (float)i;
    if (out_l) out_l[i] = ol + dg * il;
    if (out_r) out_r[i] = orr + dg * ir;
    beats_ += bpf_;
  }
  // конец блока: коэффициенты микшера, пики с плавным спадом
  const float fall = (float)std::pow(10.0, -kPeakFallDbS * (double)n / sr_ / 20.0);
  for (int k = 0; k < kTracks; ++k) {
    for (int j = 0; j < 4; ++j) mcur_[k][j] += mstep_[k][j] * (float)n;
    tpeak_[k] = std::fmax(tpeak_[k] * fall, bmax_[k]);
  }
  mpeak_[0] = std::fmax(mpeak_[0] * fall, mbl);
  mpeak_[1] = std::fmax(mpeak_[1] * fall, mbr);
  if (peaks_)  // виджет перечитает дорисованные колонки
    for (int k = idx(0); k < idx(0) + ns_; ++k)
      if (tr_[k].plive) {
        __atomic_add_fetch(&peaks_->t[k % ns_].live, 1u, __ATOMIC_RELEASE);
        tr_[k].plive = false;
      }
}

// ---------------------------------------------------------------- состояние

TrackView Looper::view(int i) const {
  const Track& t = tr_[idx(i)];
  TrackView v{};
  switch (t.st) {
    case TState::Empty: v.state = 0; break;
    case TState::Armed: v.state = 1; break;
    case TState::Rec:
    case TState::Closing: v.state = 2; break;
    case TState::Play: v.state = 3; break;
    case TState::Dub: v.state = 4; break;
  }
  v.speed = (float)t.mcur;
  v.espeed = (float)t.spd;
  v.muted = t.muted;
  v.stopped = t.stopped || t.ustop;
  v.locked = t.locked;
  v.reversed = t.dir < 0;
  v.can_undo = t.undo_n > 0 || t.st == TState::Dub;
  v.can_redo = t.redo_n > 0;
  v.layers = t.undo_n;
  if (t.len) {
    v.pos = (float)(t.pos / (double)t.len);
    v.len_s = (float)((double)t.len / sr_);
  } else if (t.st == TState::Rec || t.st == TState::Closing) {
    v.len_s = (float)((double)t.rec_frames / sr_);
  }
  return v;
}

float Looper::pool_fill() const {
  uint32_t tot = pool_.total_chunks();
  return tot ? 1.0f - (float)pool_.free_chunks() / (float)tot : 0.0f;
}

}  // namespace alp
