#include "pool.h"

#include <cstdlib>
#include <new>

namespace alp {

bool Pool::init(size_t bytes, uint32_t max_bufs) {
  release();
  uint64_t n = bytes / sizeof(Chunk);
  if (n < 16 || n > 0x7FFFFFFFu || max_bufs < 4) return false;
  mem_ = static_cast<Chunk*>(std::aligned_alloc(64, (size_t)n * sizeof(Chunk)));
  stack_ = new (std::nothrow) uint32_t[n];
  ref_ = new (std::nothrow) uint16_t[n]();
  bufs_ = new (std::nothrow) Buf[max_bufs];
  bstack_ = new (std::nothrow) int32_t[max_bufs];
  if (!mem_ || !stack_ || !ref_ || !bufs_ || !bstack_) {
    release();
    return false;
  }
  // Трогаем всю память сейчас, а не в аудиопотоке.
  std::memset(mem_, 0, (size_t)n * sizeof(Chunk));
  n_ = (uint32_t)n;
  for (uint32_t i = 0; i < n_; ++i) stack_[i] = n_ - 1 - i;  // выдаём с начала памяти
  top_ = n_;
  nbufs_ = max_bufs;
  for (uint32_t i = 0; i < nbufs_; ++i) {
    bufs_[i].used = false;
    bstack_[i] = (int32_t)(nbufs_ - 1 - i);
  }
  btop_ = nbufs_;
  return true;
}

void Pool::release() {
  std::free(mem_);
  delete[] stack_;
  delete[] ref_;
  delete[] bufs_;
  delete[] bstack_;
  mem_ = nullptr;
  stack_ = nullptr;
  ref_ = nullptr;
  bufs_ = nullptr;
  bstack_ = nullptr;
  n_ = top_ = nbufs_ = btop_ = 0;
}

int32_t Pool::alloc_buf() {
  if (!btop_) return -1;
  int32_t b = bstack_[--btop_];
  Buf& x = bufs_[b];
  for (uint32_t i = 0; i < kInline; ++i) x.inl[i] = kNone;
  x.ovf = kNone;
  x.nent = 0;
  x.len = 0;
  x.used = true;
  return b;
}

void Pool::free_buf(int32_t b) {
  if (b < 0 || !bufs_[b].used) return;
  set_len(b, 0);
  Buf& x = bufs_[b];
  free_chunk(x.ovf);
  x.ovf = kNone;
  x.used = false;
  bstack_[btop_++] = b;
}

bool Pool::set_entry(Buf& b, uint32_t i, uint32_t id) {
  if (i < kInline) {
    b.inl[i] = id;
    return true;
  }
  if (i >= kMaxBufChunks) return false;
  if (b.ovf == kNone) {
    if (id == kNone) return true;
    b.ovf = alloc_chunk();
    if (b.ovf == kNone) return false;
    uint32_t* t = table(b.ovf);
    for (uint32_t k = 0; k < kOvfEntries; ++k) t[k] = kNone;
  }
  table(b.ovf)[i - kInline] = id;
  return true;
}

bool Pool::set_len(int32_t bi, uint64_t len) {
  Buf& b = bufs_[bi];
  uint32_t ne = chunks_for(len);
  if (ne > kMaxBufChunks) return false;
  for (uint32_t i = ne; i < b.nent; ++i) {
    free_chunk(entry(b, i));
    set_entry(b, i, kNone);
  }
  for (uint32_t i = b.nent; i < ne; ++i) set_entry(b, i, kNone);
  b.nent = ne;
  b.len = len;
  return true;
}

Chunk* Pool::ensure(int32_t bi, uint32_t ci, bool zero) {
  Buf& b = bufs_[bi];
  if (ci >= kMaxBufChunks) return nullptr;
  if (ci >= b.nent) {
    for (uint32_t i = b.nent; i <= ci; ++i)
      if (!set_entry(b, i, kNone)) return nullptr;
    b.nent = ci + 1;
  }
  uint32_t id = entry(b, ci);
  if (id == kNone) {
    id = alloc_chunk();
    if (id == kNone) return nullptr;
    if (!set_entry(b, ci, id)) {
      free_chunk(id);
      return nullptr;
    }
    if (zero) std::memset(chunk(id), 0, sizeof(Chunk));
  }
  return chunk(id);
}

Chunk* Pool::writable(int32_t bi, uint32_t ci, bool zero) {
  Buf& b = bufs_[bi];
  uint32_t id = entry(b, ci);
  if (id == kNone || ref_[id] <= 1) return ensure(bi, ci, zero);
  uint32_t nid = alloc_chunk();  // кусок общий: своя копия
  if (nid == kNone) return nullptr;
  std::memcpy(chunk(nid), chunk(id), sizeof(Chunk));
  set_entry(b, ci, nid);
  free_chunk(id);
  return chunk(nid);
}

int32_t Pool::clone_buf(int32_t bi) {
  int32_t c = alloc_buf();
  if (c < 0) return -1;
  const Buf& src = bufs_[bi];
  Buf& dst = bufs_[c];
  for (uint32_t i = 0; i < src.nent; ++i) {
    uint32_t id = entry(src, i);
    if (!set_entry(dst, i, id)) {  // не хватило куска под таблицу
      dst.nent = i;
      free_buf(c);
      return -1;
    }
    if (id != kNone) ++ref_[id];
    dst.nent = i + 1;
  }
  dst.len = src.len;
  return c;
}

uint32_t Pool::used_chunks(int32_t bi) const {
  const Buf& b = bufs_[bi];
  uint32_t n = b.ovf != kNone ? 1 : 0;
  for (uint32_t i = 0; i < b.nent; ++i)
    if (entry(b, i) != kNone) ++n;
  return n;
}

}  // namespace alp
