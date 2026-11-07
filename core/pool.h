// Пул памяти под лупы: выделяется целиком при старте, дальше куски только переходят из рук в руки.
// Кусок (Chunk) = 16384 стерео-кадра. Буфер (Buf) = таблица кусков: первые 64 в самой таблице,
// остальные в отдельном куске-таблице. Пустая запись (kNone) читается как тишина.
// Свободными кусками и буферами распоряжается только аудиопоток: стек без блокировок.
// У куска счётчик ссылок: копия дорожки (Lock) делит куски с оригиналом и не тратит память,
// пока в кусок не пишут; запись в общий кусок сначала делает свою копию (copy-on-write).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace alp {

constexpr uint32_t kChunkShift = 14;
constexpr uint32_t kChunkFrames = 1u << kChunkShift;
constexpr uint32_t kChunkMask = kChunkFrames - 1;
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Chunk {
  float l[kChunkFrames];
  float r[kChunkFrames];
};

constexpr uint32_t kInline = 64;
constexpr uint32_t kOvfEntries = sizeof(Chunk) / sizeof(uint32_t);
constexpr uint32_t kMaxBufChunks = kInline + kOvfEntries;  // ~3 часа на 48 кГц

struct Buf {
  uint32_t inl[kInline];
  uint32_t ovf;      // кусок-таблица для записей с 64-й, kNone если нет
  uint32_t nent;     // записей в таблице
  uint64_t len;      // длина в кадрах
  bool used;
};

inline uint32_t chunks_for(uint64_t frames) { return (uint32_t)((frames + kChunkFrames - 1) >> kChunkShift); }

class Pool {
 public:
  Pool() = default;
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  ~Pool() { release(); }

  // Не из аудиопотока: выделяет память и сразу её трогает, чтобы потом не было подкачки страниц.
  bool init(size_t bytes, uint32_t max_bufs);
  void release();
  bool ok() const { return mem_ != nullptr; }
  size_t bytes() const { return (size_t)n_ * sizeof(Chunk); }
  // Обменяться содержимым с другим пулом (без выделения памяти; для смены размера пула).
  void swap(Pool& o);

  // ---- куски (только аудиопоток) ----
  uint32_t alloc_chunk() {
    if (!top_) return kNone;
    uint32_t id = stack_[--top_];
    ref_[id] = 1;
    return id;
  }
  void free_chunk(uint32_t id) {
    if (id != kNone && --ref_[id] == 0) stack_[top_++] = id;
  }
  uint16_t refs(uint32_t id) const { return ref_[id]; }
  Chunk* chunk(uint32_t id) const { return mem_ + id; }
  uint32_t free_chunks() const { return top_; }
  uint32_t total_chunks() const { return n_; }

  // ---- буферы ----
  int32_t alloc_buf();
  void free_buf(int32_t b);  // возвращает и все его куски
  Buf& buf(int32_t b) { return bufs_[b]; }
  const Buf& buf(int32_t b) const { return bufs_[b]; }
  uint32_t free_bufs() const { return btop_; }

  uint32_t entry(const Buf& b, uint32_t i) const {
    if (i >= b.nent) return kNone;
    if (i < kInline) return b.inl[i];
    return b.ovf == kNone ? kNone : table(b.ovf)[i - kInline];
  }
  // Задаёт длину буфера в кадрах; лишние куски возвращаются в пул. false, если не хватило таблицы.
  bool set_len(int32_t b, uint64_t len);
  // Кусок номер ci (выделяет при необходимости, zero = заполнить нулями). nullptr, если пул пуст.
  Chunk* ensure(int32_t b, uint32_t ci, bool zero);
  // То же для записи: общий с другим буфером кусок сначала копируется (copy-on-write).
  Chunk* writable(int32_t b, uint32_t ci, bool zero);
  // Новый буфер с теми же кусками (память не копируется). -1, если не хватило буферов.
  int32_t clone_buf(int32_t b);
  // Сколько кусков занимает буфер.
  uint32_t used_chunks(int32_t b) const;

  // Чтение для рабочего потока: буфер мог уже вернуться в пул и быть переиспользован,
  // поэтому проверяем номера кусков (неверные данные дадут только неточную волну).
  void read_safe(const Buf& b, uint64_t f, float& l, float& r) const {
    l = r = 0.0f;
    uint32_t i = (uint32_t)(f >> kChunkShift);
    uint32_t id;
    if (i < kInline) {
      id = b.inl[i];
    } else {
      uint32_t ov = b.ovf;
      if (ov >= n_ || i - kInline >= kOvfEntries) return;
      id = table(ov)[i - kInline];
    }
    if (id >= n_) return;
    const Chunk* c = chunk(id);
    l = c->l[f & kChunkMask];
    r = c->r[f & kChunkMask];
  }

  // Чтение кадра (пустой кусок = тишина).
  void read(const Buf& b, uint64_t f, float& l, float& r) const {
    uint32_t id = entry(b, (uint32_t)(f >> kChunkShift));
    if (id == kNone) {
      l = r = 0.0f;
      return;
    }
    const Chunk* c = chunk(id);
    l = c->l[f & kChunkMask];
    r = c->r[f & kChunkMask];
  }

 private:
  uint32_t* table(uint32_t id) const { return reinterpret_cast<uint32_t*>(mem_ + id); }
  bool set_entry(Buf& b, uint32_t i, uint32_t id);

  Chunk* mem_ = nullptr;
  uint32_t n_ = 0;
  uint32_t* stack_ = nullptr;
  uint16_t* ref_ = nullptr;
  uint32_t top_ = 0;
  Buf* bufs_ = nullptr;
  int32_t* bstack_ = nullptr;
  uint32_t nbufs_ = 0, btop_ = 0;
};

}  // namespace alp
