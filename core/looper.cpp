#include "looper.h"

#include <cmath>

namespace alp {

namespace {
constexpr uint32_t kReserve = 4;     // кусков про запас, чтобы запись не упёрлась в ноль
constexpr uint64_t kNoSeam = ~0ull;

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
  // Если столько памяти не дали, пробуем меньше: плагин должен загрузиться.
  for (size_t b = pool_bytes; b >= ((size_t)32 << 20); b /= 2)
    if (pool_.init(b, 4096)) return true;
  return false;
}

// ---------------------------------------------------------------- команды

void Looper::command(Cmd c) {
  if (!pool_.ok()) return;
  Track& t = tr_[active_];
  switch (c) {
    case Cmd::Rec: rec_press(active_); break;
    case Cmd::Undo: undo(active_); break;
    case Cmd::Redo: redo(active_); break;
    case Cmd::ResetTrack: reset_track(active_); break;
    case Cmd::ResetAll:
      if (job_pending_ || job_running_) reset_all_pending_ = true;
      else reset_all_now();
      break;
    case Cmd::Mute:
      if (t.st != TState::Play && t.st != TState::Dub) break;
      if (!set_.quantize || set_.mute_q == 0) apply_mute(active_);
      else t.mute_pending = !t.mute_pending;
      break;
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
    case Cmd::TrackNext:
    case Cmd::TrackPrev:
      leave_track(active_);
      active_ = (active_ + (c == Cmd::TrackNext ? 1 : kTracks - 1)) % kTracks;
      break;
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
    case TState::Play: start_dub(ti); break;
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
  if (set_.quantize && set_.length[ti] > 0) t.rec_target = (uint64_t)std::llround(set_.length[ti] * (double)bar_frames());
  t.st = TState::Rec;
}

void Looper::cancel_rec(int ti) {
  Track& t = tr_[ti];
  if (t.rec >= 0) pool_.free_buf(t.rec);
  t.rec = -1;
  t.rec_frames = t.rec_target = 0;
  t.st = TState::Empty;
}

void Looper::request_close(int ti) {
  Track& t = tr_[ti];
  if (!set_.quantize) {
    close_loop(ti, t.rec_frames);
    return;
  }
  if (t.rec_target) return;  // фиксированная длина: закроется сама
  // Free-длина в Quantize: конец цикла на ближайшей границе такта.
  double bf = (double)bar_frames();
  int64_t bars = std::llround((double)t.rec_frames / bf);
  if (bars < 1) bars = 1;
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
  t.xf_len = xf_;
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
  t.undo_after_dub = t.reset_after_dub = false;
  t.st = TState::Dub;
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

void Looper::reset_all_now() {
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
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
    t.dub_end = -1;
    t.undo_after_dub = t.reset_after_dub = false;
    t.seam_t = kNoSeam;
  }
}

void Looper::start_reverse(Track& t, double n) {
  t.dir = -t.dir;
  t.rate_step = 2.0 / n * t.dir;
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
  int32_t old = t.sum;
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = old;
  t.xf_owned = free_old;
  t.xf_pos = 0;
  t.xf_len = xf_;
  t.sum = new_sum;
  if (new_sum < 0) {
    t.rev_wait = -1;
    t.st = TState::Empty;
    t.len = 0;
    t.muted = t.mute_pending = false;
    t.gain = 1.0f;
    t.seam_t = kNoSeam;
    return;
  }
  t.len = pool_.buf(new_sum).len;
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
  job_ = WorkMsg{op, (uint8_t)ti, t.sum, layer, dst, L};
  job_pending_ = true;
  t.busy = true;
  return true;
}

bool Looper::take_work(WorkMsg& m) {
  if (!job_pending_) return false;
  m = job_;
  job_pending_ = false;
  job_running_ = true;
  return true;
}

void Looper::cancel_work() {
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
}

void Looper::do_work(const WorkMsg& m, WorkResp& r) {
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
  r.op = m.op;
  r.track = m.track;
}

void Looper::work_done(const WorkResp& r) {
  job_running_ = false;
  Track& t = tr_[r.track];
  t.busy = false;
  // новая сумма готова: кроссфейд со старой, старая уходит в пул
  if (t.xf_from >= 0 && t.xf_owned) pool_.free_buf(t.xf_from);
  t.xf_from = t.sum;
  t.xf_owned = true;
  t.xf_pos = 0;
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
  Chunk* c = pool_.ensure(b, (uint32_t)(p >> kChunkShift), true);
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

void Looper::dub_write(int ti, uint64_t p, float l, float r) {
  Track& t = tr_[ti];
  float env = t.dub_t < xf_ ? (float)t.dub_t / (float)xf_ : 1.0f;
  if (t.dub_end >= 0) {
    float e2 = 1.0f - (float)t.dub_end / (float)xf_;
    if (e2 < env) env = e2;
  }
  ++t.dub_t;
  Chunk* c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c && reserve(1, ti)) c = pool_.ensure(t.layer, (uint32_t)(p >> kChunkShift), true);
  if (!c) {  // память кончилась: овердаб закрывается
    end_dub(ti);
  } else {
    c->l[p & kChunkMask] += l * env;
    c->r[p & kChunkMask] += r * env;
    write_add(t.sum, p, l * env, r * env);
  }
  if (t.dub_end >= 0 && ++t.dub_end >= (int64_t)xf_) finish_dub(ti);
}

void Looper::on_grid(bool beat, bool bar) {
  for (int ti = 0; ti < kTracks; ++ti) {
    Track& t = tr_[ti];
    if (t.st == TState::Armed && bar) start_rec(ti);
    if (t.mute_pending && ((set_.mute_q == 1 && beat) || (set_.mute_q == 2 && bar))) apply_mute(ti);
  }
}

void Looper::track_sample(int ti, float il, float ir, float& ol, float& orr) {
  Track& t = tr_[ti];

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
    read_sum(t, t.xf_from, t.pos, fl, fr);
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
  ol += l * t.gain;
  orr += r * t.gain;

  // запись: хвост на стыке и овердаб
  if (playing) {
    if (t.seam_t < xf_) seam_write(t, il, ir);
    if (t.st == TState::Dub && t.layer >= 0)
      dub_write(ti, wrap_index((int64_t)std::llround(t.pos), t.len), il, ir);
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
      t.pos = std::round(t.pos);  // дальше снова прямое чтение
    }
  }
  t.pos += t.rate;
  bool wrapped = false;
  if (t.pos >= (double)L) {
    t.pos -= (double)L;
    wrapped = true;
  } else if (t.pos < 0.0) {
    t.pos += (double)L;
    wrapped = true;
  }
  if (wrapped && t.mute_pending && set_.mute_q == 3) apply_mute(ti);
}

void Looper::set_time(const TimeInfo& ti) {
  bpf_ = ti.beats_per_frame;
  bpb_ = ti.bpb > 0 ? ti.bpb : 4;
  rolling_ = ti.rolling;
  if (rolling_) beats_ = ti.beats;  // сетка Zynthian; иначе продолжаем свой счёт
}

void Looper::process(const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n,
                     const TimeInfo& ti) {
  set_time(ti);
  const float dry = set_.dry;
  for (uint32_t i = 0; i < n; ++i) {
    const double b0 = beats_;
    beats_ += bpf_;
    if (set_.quantize) {
      const int64_t a = (int64_t)std::floor(b0), c = (int64_t)std::floor(beats_);
      if (a != c) on_grid(true, ((c % bpb_) + bpb_) % bpb_ == 0);
    } else {
      for (int k = 0; k < kTracks; ++k)
        if (tr_[k].st == TState::Armed) start_rec(k);
    }
    const float il = in_l ? in_l[i] : 0.0f;
    const float ir = in_r ? in_r[i] : 0.0f;
    float ol = 0.0f, orr = 0.0f;
    if (pool_.ok())
      for (int k = 0; k < kTracks; ++k) track_sample(k, il, ir, ol, orr);
    if (out_l) out_l[i] = ol + dry * il;
    if (out_r) out_r[i] = orr + dry * ir;
  }
}

// ---------------------------------------------------------------- состояние

TrackView Looper::view(int i) const {
  const Track& t = tr_[i];
  TrackView v{};
  switch (t.st) {
    case TState::Empty: v.state = 0; break;
    case TState::Armed: v.state = 1; break;
    case TState::Rec:
    case TState::Closing: v.state = 2; break;
    case TState::Play: v.state = 3; break;
    case TState::Dub: v.state = 4; break;
  }
  v.muted = t.muted;
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
