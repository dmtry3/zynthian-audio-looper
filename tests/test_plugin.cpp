// Тесты LV2-обёртки без хоста: ручки страницы 5 с захватом (catch), смена сетки.
#include <cstdio>
#include <initializer_list>

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
  // шкала ручек Level и Master: 75 = 0 дБ, 100 = +6, 0 = −∞, около 0 дБ мелко
  CHECK(std::fabs(pos_to_db(75.0f)) < 1e-6f && std::fabs(pos_to_db(100.0f) - 6.0f) < 1e-5f);
  CHECK(pos_to_db(0.0f) == kMinDb);
  bool mono = true;
  for (int i = 1; i <= 127; ++i)
    if (pos_to_db(i * 100.0f / 127.0f) < pos_to_db((i - 1) * 100.0f / 127.0f)) mono = false;
  CHECK(mono);
  // шаг MIDI (1/127) около 0 дБ: снизу меньше 0,1 дБ, сверху около 0,2 дБ
  const float step = 100.0f / 127.0f;
  CHECK(pos_to_db(75.0f) - pos_to_db(75.0f - step) < 0.1f);
  CHECK(pos_to_db(75.0f + step) - pos_to_db(75.0f) < 0.25f);
  for (float db : {-40.0f, -12.0f, -3.0f, -0.5f, 0.0f, 3.0f})
    CHECK(std::fabs(pos_to_db(db_to_pos(db)) - db) < 1e-3f);
  // смена сетки пользователем: ядро в 6×6, микшер и скорости дорожек по умолчанию, Dry остаётся
  {
    init_params();
    Plugin pl;
    pl.looper = new Looper();
    pl.looper->init(48000.0, (size_t)64 << 20);
    for (int i = 0; i < PR_COUNT; ++i) pl.vals[i] = kParams[i].def;
    pl.apply_all();
    CHECK(pl.looper->grid() == 4);
    set_param(&pl, PR_LEV0 + 5, -12.0);
    on_param(&pl, PR_LEV0 + 5);
    set_param(&pl, PR_SPD0 + 2, 0.5);
    on_param(&pl, PR_SPD0 + 2);
    set_param(&pl, PR_DRY, -6.0);
    on_param(&pl, PR_DRY);
    CHECK(pl.looper->mix(5).level_db == -12.0f && pl.looper->speed(2) == 0.5f);
    set_param(&pl, PR_GRID, 2);
    on_param(&pl, PR_GRID);
    CHECK(pl.looper->grid() == 6);
    CHECK(pl.vals[PR_LEV0 + 5] == 0.0f && pl.looper->mix(5).level_db == 0.0f && pl.dirty[PR_LEV0 + 5]);
    CHECK(pl.vals[PR_SPD0 + 2] == 1.0f && pl.looper->speed(2) == 1.0f);
    CHECK(pl.vals[PR_DRY] == -6.0f && pl.looper->dry_db() == -6.0f);
    // повторно та же сетка — микшер не трогается
    set_param(&pl, PR_LEV0 + 30, -3.0);
    on_param(&pl, PR_LEV0 + 30);
    set_param(&pl, PR_GRID, 2);
    on_param(&pl, PR_GRID);
    CHECK(pl.looper->mix(30).level_db == -3.0f);
    // снапшот (restore -> apply_all) с другой сеткой: сетка меняется, его микшер остаётся
    pl.vals[PR_GRID] = 0;
    pl.vals[PR_LEV0 + 1] = -9.0f;
    pl.apply_all();
    CHECK(pl.looper->grid() == 2 && pl.looper->mix(1).level_db == -9.0f);
    // команды виджета для дорожек 5 и 6 в 6×6
    pl.vals[PR_GRID] = 2;
    pl.apply_all();
    set_param(&pl, PR_TMUTE, 6);
    CHECK(pl.vals[PR_TMUTE] == 6.0f);
    on_param(&pl, PR_TMUTE);
    CHECK(pl.vals[PR_TMUTE] == 0.0f);
    // настройки этапа 6: долей в такте, задержка, пороги жестов
    set_param(&pl, PR_BPB, 3);
    on_param(&pl, PR_BPB);
    set_param(&pl, PR_LAT, 25);
    on_param(&pl, PR_LAT);
    CHECK(pl.clock.bpb() == 3 && pl.looper->settings().latency_ms == 25.0f);
    set_param(&pl, PR_LAT, 500);  // за пределом — до 100
    CHECK(pl.vals[PR_LAT] == 100.0f);
    set_param(&pl, PR_HOLD, 300);
    on_param(&pl, PR_HOLD);
    auto hold_after = [&](uint32_t b, uint32_t blocks) {  // удержание кнопки b: через сколько блоков Hold
      Button& btn = pl.buttons[b];
      btn.process(0.0f, 128);
      btn.process(0.0f, 128);
      for (uint32_t k = 1; k <= blocks; ++k)
        if (btn.process(1.0f, 128) == Gesture::Hold) return k;
      return 0u;
    };
    const uint32_t undo_b = P_UNDO - P_REC, reset_b = P_RESET - P_REC;
    CHECK(hold_after(undo_b, 1000) == 114);   // 300 мс = 112,5 блока по 128 (+ блок нажатия)
    CHECK(hold_after(reset_b, 1000) == 376);  // Reset — свой порог, 1 с
    set_param(&pl, PR_RHOLD, 2000);
    on_param(&pl, PR_RHOLD);
    pl.buttons[reset_b].process(0.0f, 128);
    CHECK(hold_after(reset_b, 1000) == 751);
    delete pl.looper;
  }
  std::printf("plugin: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
