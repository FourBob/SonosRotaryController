#include "Encoder.h"
#include <Arduino.h>

extern volatile int encoder_counter;
extern volatile int move_flag;

namespace inputs {

EncoderRotary* EncoderRotary::self_ = nullptr;

void EncoderRotary::begin(int pinA, int pinB) {
  pinA_ = pinA; pinB_ = pinB;
  pinMode(pinA_, INPUT_PULLUP);
  pinMode(pinB_, INPUT_PULLUP);
  lastA_ = digitalRead(pinA_);
  self_ = this;
}

void IRAM_ATTR EncoderRotary::isr_trampoline() {
  if (self_) self_->onEdge();
}

void IRAM_ATTR EncoderRotary::onEdge() {
  int s = digitalRead(pinA_);
  if (s != lastA_) {
    if (digitalRead(pinB_) == s) encoder_counter++; else encoder_counter--;
    lastA_ = s;
    move_flag = 1;
  }
}

} // namespace inputs

