// Распознавание тапа и удержания для контроллеров-действий (0 и 1).
// Без LV2, без выделения памяти: безопасно вызывать из аудиопотока.
#pragma once
#include <cstdint>

namespace alp {

enum class Gesture : uint8_t { None, Tap, Hold };

// Одна кнопка. Значение контроллера приходит раз в аудиоблок.
//  - Кнопка без удержания: Tap сразу по переходу 0->1, отпускание ничего не делает.
//  - Кнопка с удержанием: Hold срабатывает, как только кнопка продержана порог
//    (не дожидаясь отпускания). Tap срабатывает при отпускании раньше порога.
class Button {
 public:
  void configure(bool has_hold, uint32_t hold_samples) {
    has_hold_ = has_hold;
    hold_samples_ = hold_samples;
  }

  // value: текущее значение контроллера, frames: длина блока в сэмплах.
  Gesture process(float value, uint32_t frames) {
    const bool down = value >= 0.5f;
    Gesture g = Gesture::None;
    if (!primed_) {
      // Первый блок только запоминает положение: значение 1, восстановленное
      // из снапшота, не должно сработать как нажатие.
      primed_ = true;
      down_ = down;
      fired_ = true;
      return g;
    }
    if (down && !down_) {
      down_ = true;
      held_ = 0;
      fired_ = false;
      if (!has_hold_) g = Gesture::Tap;
    } else if (!down && down_) {
      down_ = false;
      if (has_hold_ && !fired_) g = Gesture::Tap;
    } else if (down && has_hold_ && !fired_) {
      held_ += frames;
      if (held_ >= hold_samples_) {
        fired_ = true;
        g = Gesture::Hold;
      }
    }
    return g;
  }

  bool down() const { return down_; }

 private:
  bool primed_ = false;
  bool has_hold_ = false;
  uint32_t hold_samples_ = 0;
  bool down_ = false;
  bool fired_ = false;
  uint64_t held_ = 0;
};

}  // namespace alp
