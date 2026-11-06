#include "peaks.h"

#include <cstring>

namespace alp {

void clear_wave(PeakWave& out) {
  std::memset(out.mn, 0, sizeof out.mn);
  std::memset(out.mx, 0, sizeof out.mx);
}

void compute_wave(const Pool& pool, int32_t buf, uint64_t len, PeakWave& out) {
  if (buf < 0 || len == 0) {
    clear_wave(out);
    return;
  }
  const Buf& b = pool.buf(buf);
  for (uint32_t c = 0; c < kPeakCols; ++c) {
    const uint64_t a = len * c / kPeakCols, e = len * (c + 1) / kPeakCols;
    float mn = 0.0f, mx = 0.0f;
    for (uint64_t f = a; f < e; ++f) {
      float l, r;
      pool.read_safe(b, f, l, r);
      if (l < mn) mn = l;
      if (r < mn) mn = r;
      if (l > mx) mx = l;
      if (r > mx) mx = r;
    }
    out.mn[c] = peak_q(mn);
    out.mx[c] = peak_q(mx);
  }
}

}  // namespace alp
