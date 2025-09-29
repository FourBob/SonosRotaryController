#pragma once
#include <cstdint>
#include <cstddef>

struct DecodeResult {
  int w = 0;
  int h = 0;
  bool ok = false;
};

struct IImageDecoder {
  virtual ~IImageDecoder() {}
  virtual DecodeResult sizeOf(const uint8_t* d, size_t n) = 0;
  // Decode entire image into provided buffer (RGB565). Buffer size must be w*h.
  virtual bool decodeToRGB565(const uint8_t* d, size_t n, uint16_t* out, int w, int h) = 0;
};

