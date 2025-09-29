#pragma once
#include <Arduino_GFX_Library.h>
#include "base/Config.h"

namespace ui_gfx {

class Display {
public:
  // Wrap an existing Arduino_RGB_Display*
  explicit Display(Arduino_RGB_Display* g) : g_(g) {}

  void fill(uint16_t color) { if (g_) g_->fillScreen(color); }

  // Draw a single RGB565 row
  void drawRow(int x, int y, const uint16_t* data, int w) {
    if (!g_ || w <= 0) return;
    g_->draw16bitRGBBitmap(x, y, (uint16_t*)data, (uint16_t)w, 1);
  }

  Arduino_RGB_Display* raw() const { return g_; }

private:
  Arduino_RGB_Display* g_ = nullptr;
};

} // namespace ui_gfx

