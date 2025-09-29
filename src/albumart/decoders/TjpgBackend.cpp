#include <Arduino.h>
#include <TJpg_Decoder.h>
#include "IImageDecoder.h"

namespace {
static uint16_t* g_fb = nullptr; static int g_w=0, g_h=0; static int g_off_x=0, g_off_y=0;
static bool cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!g_fb) return true;
  int src_stride = (int)w; // original block width
  // Start positions in destination and offsets into source
  int dx = x + g_off_x;
  int dy = y + g_off_y;
  int sx_off = 0;
  int sy_off = 0;
  int copy_w = (int)w;
  int copy_h = (int)h;
  // Handle negative dest coords by skipping pixels/rows from source
  if (dx < 0) { sx_off = -dx; dx = 0; copy_w -= sx_off; }
  if (dy < 0) { sy_off = -dy; dy = 0; copy_h -= sy_off; }
  if (copy_w <= 0 || copy_h <= 0) return true;
  // Clip to framebuffer bounds
  if (dx >= g_w || dy >= g_h) return true;
  if (dx + copy_w > g_w) copy_w = g_w - dx;
  if (dy + copy_h > g_h) copy_h = g_h - dy;
  if (copy_w <= 0 || copy_h <= 0) return true;
  // Source start pointer after skips
  const uint16_t* src = bmp + sy_off * src_stride + sx_off;
  for (int row = 0; row < copy_h; ++row) {
    const uint16_t* s = src + row * src_stride;
    uint16_t* d = &g_fb[(dy + row) * g_w + dx];
    memcpy(d, s, (size_t)copy_w * sizeof(uint16_t));
  }
  return true;
}
}

class TjpgBackend : public IImageDecoder {
public:
  DecodeResult sizeOf(const uint8_t* d, size_t n) override {
    DecodeResult r; if (!d || n < 4) return r;
    uint16_t w=0,h=0;
    auto rc = TJpgDec.getJpgSize(&w,&h,d,n);
    if (rc == JDR_OK) { r.w=w; r.h=h; r.ok=true; }
    else Serial.printf("Tjpg: getJpgSize failed rc=%d\n", (int)rc);
    return r;
  }
  bool decodeToRGB565(const uint8_t* d, size_t n, uint16_t* out, int w, int h) override {
    if (!d || !out) { Serial.println("Tjpg: invalid args to decode"); return false; }
    // Determine source dimensions to center without scaling
    uint16_t srcw=0, srch=0;
    auto szrc = TJpgDec.getJpgSize(&srcw, &srch, d, n);
    if (szrc != JDR_OK) { Serial.printf("Tjpg: getJpgSize in decode failed rc=%d\n", (int)szrc); return false; }
    g_off_x = (int)w/2 - (int)srcw/2;
    g_off_y = (int)h/2 - (int)srch/2;

    g_fb = out; g_w=w; g_h=h;
    TJpgDec.setSwapBytes(false);
    TJpgDec.setJpgScale(0); // 0 = full resolution
    TJpgDec.setCallback(cb);
    Serial.printf("Tjpg: drawJpg into %dx%d (src %ux%u, off %d,%d) ...\n", w, h, srcw, srch, g_off_x, g_off_y);
    JRESULT rc = TJpgDec.drawJpg(0,0,d,n);
    bool ok = (rc == JDR_OK);
    if (!ok) {
      Serial.printf("Tjpg: drawJpg failed rc=%d\n", (int)rc);
    } else {
      Serial.println("Tjpg: drawJpg OK");
    }
    g_fb=nullptr; g_w=g_h=0; g_off_x=g_off_y=0;
    return ok;
  }
};

extern "C" IImageDecoder* createTjpgDecoder() { return new TjpgBackend(); }

