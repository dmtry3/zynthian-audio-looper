// Тесты LV2-обёртки без хоста: ручки страницы 5 с захватом (catch).
#include <cstdio>

#include "../lv2/audio_looper.cpp"

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

int main() {
  Plugin::Catch c;
  // первое значение только запоминается
  CHECK(!catch_knob(c, 0.0f, 0, 0.0f, 0.5f));
  // дорожка 1 на 0 дБ: педаль проходит через 0 — сразу ведёт
  CHECK(catch_knob(c, -1.0f, 0, 0.0f, 0.5f) && c.applied == -1.0f);
  CHECK(catch_knob(c, -20.0f, 0, -1.0f, 0.5f) && c.applied == -20.0f);
  // переключились на дорожку 2 (0 дБ): педаль на −20, двигаем к −25 и −10 — не захватывает
  CHECK(!catch_knob(c, -25.0f, 1, 0.0f, 0.5f));
  CHECK(!catch_knob(c, -10.0f, 1, 0.0f, 0.5f));
  // дошли до уровня дорожки (перешли через 0) — захватила и ведёт
  CHECK(catch_knob(c, 0.3f, 1, 0.0f, 0.5f) && c.applied == 0.3f);
  CHECK(catch_knob(c, -3.0f, 1, 0.3f, 0.5f) && c.applied == -3.0f);
  // уровень изменили не педалью (виджет: −12) — педаль снова ждёт захвата
  CHECK(!catch_knob(c, -2.0f, 1, -12.0f, 0.5f));
  CHECK(catch_knob(c, -12.2f, 1, -12.0f, 0.5f));
  std::printf("plugin: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
