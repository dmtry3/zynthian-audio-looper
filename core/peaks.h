// Пики для волн виджета: минимум и максимум по колонкам цикла.
// Лежат в общей памяти (файл /dev/shm/audio_looper_<pid>, его создаёт LV2-обёртка),
// виджет читает их без блокировок. Запись под счётчиком seq: нечётный — идёт запись.
// Полностью пересчитывает рабочий поток (при окончании записи, овердаба, undo, redo, reset);
// аудиопоток только дорисовывает колонки под линией позиции во время овердаба (счётчик live).
#pragma once
#include <cstdint>

#include "pool.h"

namespace alp {

constexpr uint32_t kPeakCols = 512;
constexpr uint32_t kPeakLayers = 8;            // последние слои овердабов, видимые отдельно
constexpr uint32_t kPeakMagic = 0x31504C41u;   // "ALP1"

struct PeakWave {
  int8_t mn[kPeakCols];
  int8_t mx[kPeakCols];
};

struct PeakTrack {
  uint32_t seq;                      // нечётный — рабочий поток пишет
  uint32_t live;                     // растёт, когда аудиопоток дорисовал колонки
  uint32_t nlayers;                  // сколько слоёв в layer[] (новые первыми)
  uint32_t dub_active;               // идёт овердаб: волна dub растёт вживую
  uint32_t layer_no[kPeakLayers];    // номер слоя с начала дорожки (для постоянного цвета)
  PeakWave sum;                      // всё, что играет
  PeakWave dub;                      // текущий овердаб
  PeakWave layer[kPeakLayers];       // слои овердабов, новые первыми
};

struct PeakShm {
  uint32_t magic, ncols, ntracks, nlayers;
  PeakTrack t[4];
};

static_assert(sizeof(PeakTrack) == 16 + 4 * kPeakLayers + 2 * kPeakCols * (2 + kPeakLayers), "раскладка для виджета");

inline int8_t peak_q(float v) {
  float x = v * 127.0f;
  if (x > 127.0f) x = 127.0f;
  if (x < -127.0f) x = -127.0f;
  return (int8_t)(x < 0 ? x - 0.5f : x + 0.5f);
}

// Пики буфера по колонкам (рабочий поток). Буфер мог уже уйти в пул — читаем безопасно.
void compute_wave(const Pool& pool, int32_t buf, uint64_t len, PeakWave& out);
void clear_wave(PeakWave& out);

inline void seq_begin(PeakTrack& t) { __atomic_store_n(&t.seq, t.seq | 1u, __ATOMIC_RELEASE); }
inline void seq_end(PeakTrack& t) { __atomic_store_n(&t.seq, (t.seq | 1u) + 1u, __ATOMIC_RELEASE); }

}  // namespace alp
