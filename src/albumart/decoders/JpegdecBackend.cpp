#include <Arduino.h>
#include <JPEGDEC.h>
#include "IImageDecoder.h"

namespace {
static uint16_t* g_fb = nullptr;
static int g_fb_w = 0, g_fb_h = 0;
static int g_off_x = 0, g_off_y = 0;

static int draw_cb(JPEGDRAW* p) {
  if (!g_fb) return 0;
  int src_stride = (int)((p->iWidth > 0) ? p->iWidth : p->iWidthUsed);
  int dx = p->x + g_off_x;
  int dy = p->y + g_off_y;
  int sx_off = 0;
  int sy_off = 0;
  int copy_w = (p->iWidthUsed > 0) ? p->iWidthUsed : p->iWidth;
  int copy_h = p->iHeight;
  // Adjust for negative destination
  if (dx < 0) { sx_off = -dx; dx = 0; copy_w -= sx_off; }
  if (dy < 0) { sy_off = -dy; dy = 0; copy_h -= sy_off; }
  if (copy_w <= 0 || copy_h <= 0) return 1;
  // Clip to framebuffer bounds
  if (dx >= g_fb_w || dy >= g_fb_h) return 1;
  if (dx + copy_w > g_fb_w) copy_w = g_fb_w - dx;
  if (dy + copy_h > g_fb_h) copy_h = g_fb_h - dy;
  if (copy_w <= 0 || copy_h <= 0) return 1;
  // Source start
  const uint16_t* src = (const uint16_t*)p->pPixels;
  src += sy_off * src_stride + sx_off;
  for (int row = 0; row < copy_h; ++row) {
    const uint16_t* s = src + row * src_stride;
    uint16_t* d = &g_fb[(dy + row) * g_fb_w + dx];
    memcpy(d, s, (size_t)copy_w * sizeof(uint16_t));
  }
  return 1;
}
}

class JpegdecBackend : public IImageDecoder {
public:
  DecodeResult sizeOf(const uint8_t* d, size_t n) override {
    DecodeResult r; if (!d || n < 4) return r;
    JPEGDEC j; if (j.openRAM((uint8_t*)d, (int)n, nullptr) != JPEG_SUCCESS) return r;
    r.w = j.getWidth(); r.h = j.getHeight(); r.ok = (r.w > 0 && r.h > 0);
    j.close(); return r;
  }
  bool decodeToRGB565(const uint8_t* d, size_t n, uint16_t* out, int w, int h) override {
    if (!d || !out || w <= 0 || h <= 0) return false;
    JPEGDEC j; if (j.openRAM((uint8_t*)d, (int)n, draw_cb) != JPEG_SUCCESS) return false;
    j.setPixelType(RGB565_LITTLE_ENDIAN);
    // Center the image without scaling
    int srcw = j.getWidth();
    int srch = j.getHeight();
    g_off_x = (int)w/2 - srcw/2;
    g_off_y = (int)h/2 - srch/2;
    Serial.printf("Jpegdec: decode into %dx%d (src %dx%d, off %d,%d) ...\n", w, h, srcw, srch, g_off_x, g_off_y);
    g_fb = out; g_fb_w = w; g_fb_h = h;
    int rc = j.decode(0, 0, 0);
    int err = j.getLastError();
    j.close();
    bool ok = (rc == JPEG_SUCCESS && err == JPEG_SUCCESS);
    if (!ok) Serial.printf("Jpegdec: decode failed rc=%d err=%d\n", rc, err);
    else Serial.println("Jpegdec: decode OK");
    g_fb = nullptr; g_fb_w = g_fb_h = 0; g_off_x = g_off_y = 0;
    return ok;
  }
};

// Factory helper
extern "C" IImageDecoder* createJpegdecDecoder() { return new JpegdecBackend(); }

