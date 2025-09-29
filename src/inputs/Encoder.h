#pragma once
#include <Arduino.h>

// Use existing globals for compatibility
extern volatile int encoder_counter;
extern volatile int move_flag;

namespace inputs {

class EncoderRotary {
public:
  void begin(int pinA, int pinB);
  static void IRAM_ATTR isr_trampoline();

private:
  void IRAM_ATTR onEdge();

  int pinA_ = -1;
  int pinB_ = -1;
  volatile int lastA_ = 0;
  static EncoderRotary* self_;
};

} // namespace inputs

