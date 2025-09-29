#pragma once
#include <Arduino.h>
#include <Wire.h>

namespace inputs {

class TouchCST826 {
public:
  // Returns true only on first touch (rising edge). Sets x,y to touch position.
  bool readTap(int &x, int &y) {
    uint8_t data_raw[7] = {0};
    if (i2cRead(0x15, 0x02, data_raw, 7) != 0) return false;
    int event = data_raw[1] >> 6; // 0=Down, 1=Up, 2=Contact
    bool touching = (event == 0 || event == 2);
    if (touching) {
      x = (int)data_raw[2] + (int)(data_raw[1] & 0x0F) * 256;
      y = (int)data_raw[4] + (int)(data_raw[3] & 0x0F) * 256;
      if (!touch_active_) { touch_active_ = true; return true; }
      return false;
    }
    if (event == 1) touch_active_ = false; // Up
    return false;
  }

private:
  static int i2cRead(uint16_t addr, uint8_t reg_addr, uint8_t *reg_data, uint32_t length) {
    Wire.beginTransmission(addr);
    Wire.write(reg_addr);
    if (Wire.endTransmission(true)) return -1;
    Wire.requestFrom((uint16_t)addr, (uint8_t)length, (bool)true);
    for (uint32_t i = 0; i < length && Wire.available(); i++) {
      *reg_data++ = Wire.read();
    }
    return 0;
  }

  bool touch_active_ = false;
};

} // namespace inputs

