#pragma once
#include <Arduino.h>
#include <vector>
#include "base/Config.h"
#include "gfx/Display.h"
#include "albumart/AlbumArtRenderer.h"
#include "albumart/decoders/IImageDecoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Factories implemented by backends
extern "C" IImageDecoder* createJpegdecDecoder();
extern "C" IImageDecoder* createTjpgDecoder();

namespace albumart {

class AlbumArtService {
public:
  // Global decoder mutex to serialize non-reentrant backends
  static SemaphoreHandle_t decoderMutex() {
    static SemaphoreHandle_t mtx = [](){ return xSemaphoreCreateMutex(); }();
    return mtx;
  }

  // RAII guard
  struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mtx): m(mtx) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock(){ if (m) xSemaphoreGive(m); }
  };

  // Decode bytes in RAM, center-crop/pad to 480x480, return in dst480 (no drawing)
  static bool decodeToCropped480(const uint8_t* data, size_t n, uint16_t* dst480) {
    if (!data || n == 0 || !dst480) { Serial.println("AlbumArt: decodeToCropped480 invalid args"); return false; }
    Lock guard(decoderMutex()); // serialize all decode operations

    unsigned long t0 = millis();
    std::unique_ptr<IImageDecoder> tjpg(createTjpgDecoder());
    std::unique_ptr<IImageDecoder> jp(createJpegdecDecoder());

    // Determine source size (prefer TJPG, fallback to JPEGDEC)
    Serial.printf("AlbumArt: sizeOf on %u bytes...\n", (unsigned)n);
    DecodeResult sz = tjpg->sizeOf(data, n);
    if (!sz.ok) {
      sz = jp->sizeOf(data, n);
      if (!sz.ok) { Serial.println("AlbumArt: sizeOf failed (both backends)"); return false; }
    }
    Serial.printf("AlbumArt: source dims %dx%d\n", sz.w, sz.h);

    // Try TJPG first (fast, low RAM), then JPEGDEC as fallback for progressive JPEGS
    unsigned long t1 = millis();
    bool ok = tjpg->decodeToRGB565(data, n, dst480, sys::kScreenW, sys::kScreenH);
    unsigned long t2 = millis();
    if (ok) {
      Serial.printf("AlbumArt: decode ok (TJPG, centered no-scale) in %lu ms (total %lu ms)\n", (t2 - t1), (t2 - t0));
      return true;
    }
    Serial.println("AlbumArt: TJPG failed, trying JPEGDEC fallback...");

    unsigned long t3 = millis();
    bool ok2 = jp->decodeToRGB565(data, n, dst480, sys::kScreenW, sys::kScreenH);
    unsigned long t4 = millis();
    if (ok2) {
      Serial.printf("AlbumArt: decode ok (JPEGDEC fallback, centered no-scale) in %lu ms (total %lu ms)\n", (t4 - t3), (t4 - t0));
      return true;
    }

    Serial.println("AlbumArt: decode failed on both backends");
    return false;
  }

  // Legacy convenience: decode and draw via display
  static bool drawForegroundFromBytes(ui_gfx::Display& disp, const uint8_t* data, size_t n) {
    if (!data || n == 0) return false;
    std::vector<uint16_t> dst((size_t)sys::kScreenW * (size_t)sys::kScreenH);
    if (!decodeToCropped480(data, n, dst.data())) return false;
    for (int y = 0; y < sys::kScreenH; ++y) {
      disp.drawRow(0, y, &dst[(size_t)y * sys::kScreenW], sys::kScreenW);
    }
    return true;
  }
};

} // namespace albumart

