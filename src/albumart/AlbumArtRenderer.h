#pragma once
#include <cstdint>
#include <cstddef>
#include "base/Config.h"

namespace albumart {

// Copies the centered crop/pad from src (sw x sh) into dst (480x480 RGB565)
inline void centerCropPadTo480(const uint16_t* src, int sw, int sh, uint16_t* dst480) {
  const int DW = sys::kScreenW, DH = sys::kScreenH;
  if (!src || !dst480 || sw <= 0 || sh <= 0) return;
  // Clear destination (black)
  for (int i = 0; i < DW*DH; ++i) dst480[i] = 0x0000;
  const int src_x0 = (sw > DW) ? (sw - DW) / 2 : 0;
  const int src_y0 = (sh > DH) ? (sh - DH) / 2 : 0;
  const int copy_w = (sw > DW) ? DW : sw;
  const int copy_h = (sh > DH) ? DH : sh;
  const int dst_x0 = (sw < DW) ? (DW - sw) / 2 : 0;
  const int dst_y0 = (sh < DH) ? (DH - sh) / 2 : 0;
  for (int row = 0; row < copy_h; ++row) {
    const uint16_t* s = &src[(src_y0 + row) * sw + src_x0];
    uint16_t* d = &dst480[(dst_y0 + row) * DW + dst_x0];
    memcpy(d, s, (size_t)copy_w * sizeof(uint16_t));
  }
}

} // namespace albumart

