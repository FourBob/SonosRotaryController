#include "albumart/BackgroundArt.h"
#include "albumart/Downloader.h"
#include "albumart/AlbumArtService.h"
#include <FS.h>
#include <SPIFFS.h>
#include <PNGdec.h>
#include <TJpg_Decoder.h>
#include <JPEGDEC.h>
#include <vector>
#include "esp_heap_caps.h"

namespace albumart {

// Static decode context for TJpg callback
namespace {
  static BackgroundArt* s_curr = nullptr;
  static int s_src_w=0, s_src_h=0;
  static int s_src_x0=0, s_src_y0=0;
  static int s_dst_x0=0, s_dst_y0=0;
  static int s_crop_w=0, s_crop_h=0;
  static bool tjpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
    if (!s_curr) return true;
    uint16_t* fb = s_curr->fbRaw();
    if (!fb || w==0 || h==0) return true;
    int bx0 = x, by0 = y, bx1 = x + (int)w, by1 = y + (int)h;
    int ix0 = (bx0 < s_src_x0) ? s_src_x0 : bx0;
    int iy0 = (by0 < s_src_y0) ? s_src_y0 : by0;
    int ix1 = (bx1 > s_src_x0 + s_crop_w) ? (s_src_x0 + s_crop_w) : bx1;
    int iy1 = (by1 > s_src_y0 + s_crop_h) ? (s_src_y0 + s_crop_h) : by1;
    if (ix0 >= ix1 || iy0 >= iy1) return true;
    int copy_w = ix1 - ix0;
    for (int yy = iy0; yy < iy1; ++yy) {
      int src_row_off = (yy - by0) * (int)w + (ix0 - bx0);
      const uint16_t* src = bmp + src_row_off;
      int dst_x = s_dst_x0 + (ix0 - s_src_x0);
      int dst_y = s_dst_y0 + (yy - s_src_y0);
      // Hard safety clamp to avoid any OOB writes
      if (dst_y < 0 || dst_y >= 480) continue;
      if (dst_x < 0) {
        int delta = -dst_x; dst_x = 0; src += delta; copy_w -= delta;
      }
      if (dst_x + copy_w > 480) {
        int keep = 480 - dst_x; if (keep <= 0) continue; copy_w = keep;
      }
      uint16_t* dst = &fb[dst_y * 480 + dst_x];
      memcpy(dst, src, (size_t)copy_w * sizeof(uint16_t));
    }
    return true;
  }
}

void BackgroundArt::start() {
  if (job_busy_) return;
  if (url_.length() && url_ == last_started_url_) { Serial.println("AlbumArt(bg): URL unchanged, skip start"); return; }
  job_busy_ = true;
  file_ready_ = false;
  ready_ = false;
  blit_row_ = 0;

  // Start downloader task (Core 0)
  struct Ctx { BackgroundArt* self; } *ctx = new Ctx{this};
  auto dlTask = [](void* arg){
    std::unique_ptr<Ctx> holder((Ctx*)arg);
    BackgroundArt* self = holder->self;
    // Require a URL; do not fallback to any preset
    if (!self->url_.length()) {
      Serial.println("AlbumArt(bg): no URL provided, skipping download");
      self->file_ready_ = false;
      self->job_busy_ = false;
      vTaskDelete(NULL);
      return;
    }
    String eff = self->url_;
    Serial.printf("AlbumArt(bg): downloading %s\n", eff.c_str());
    self->last_started_url_ = eff;
    if (SPIFFS.exists(self->path_)) SPIFFS.remove(self->path_);
    bool ok = Downloader::downloadToFile(eff, self->path_);
    if (ok) {
      File f = SPIFFS.open(self->path_, FILE_READ);
      int sz = f ? f.size() : -1; if (f) f.close();
      Serial.printf("AlbumArt(bg): saved %d bytes to %s\n", sz, self->path_);
    } else {
      Serial.println("AlbumArt(bg): download failed");
    }
    self->file_ready_ = SPIFFS.exists(self->path_) && (SPIFFS.open(self->path_, FILE_READ).size() > 0);
    self->job_busy_ = false;
    vTaskDelete(NULL);
  };
  const uint32_t stack_words = 16384; // 64KB for HTTPS/TLS handshake + file IO
  xTaskCreatePinnedToCore(dlTask, "aa_bg", stack_words, ctx, tskIDLE_PRIORITY+2, nullptr, 1);
}

void BackgroundArt::tick() {
  if (file_ready_ && !decode_busy_) {
    file_ready_ = false;
    decode_busy_ = true;
    // Spawn decode worker
    struct Ctx { BackgroundArt* self; } *ctx = new Ctx{this};
    auto decTask = [](void* arg){
      std::unique_ptr<Ctx> holder((Ctx*)arg);
      BackgroundArt* self = holder->self;
      char bg_reason[96]; strncpy(bg_reason, "unknown", sizeof(bg_reason));
      self->ready_ = false;

      // Read file into RAM
      std::vector<uint8_t> jpg;
      File fi = SPIFFS.open(self->path_, FILE_READ);
      if (fi) {
        size_t n = fi.size(); jpg.resize(n);
        if (n) (void)fi.read(jpg.data(), n);
        fi.close();
        Serial.printf("AlbumArt(bg): file read %u bytes, head=%02X %02X %02X %02X\n", (unsigned)jpg.size(), jpg.size()>0?jpg[0]:0, jpg.size()>1?jpg[1]:0, jpg.size()>2?jpg[2]:0, jpg.size()>3?jpg[3]:0);
      }

      // Allocate internal FB
      if (self->fb_) { heap_caps_free(self->fb_); self->fb_ = nullptr; }
      self->fb_ = (uint16_t*) heap_caps_malloc(480*480*sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!self->fb_) self->fb_ = (uint16_t*) heap_caps_malloc(480*480*sizeof(uint16_t), MALLOC_CAP_8BIT);
      if (!self->fb_) self->fb_ = (uint16_t*) malloc(480*480*sizeof(uint16_t));
      if (self->fb_) memset(self->fb_, 0x00, 480*480*sizeof(uint16_t));

      if (!jpg.empty() && self->fb_) {
        bool is_png = (jpg.size() >= 8 && jpg[0]==0x89 && jpg[1]==0x50 && jpg[2]==0x4E && jpg[3]==0x47 && jpg[4]==0x0D && jpg[5]==0x0A && jpg[6]==0x1A && jpg[7]==0x0A);
        bool is_jpeg = (jpg.size() >= 2 && jpg[0]==0xFF && jpg[1]==0xD8);
        // Temporary: disable PNG path during stabilization (suspected misuse of PNGdec line API)
        if (false && is_png) {
          // PNG decode temporarily disabled
        } else if (is_jpeg) {
          // Use the proven AlbumArtService pipeline to decode into our framebuffer
          bool ok = albumart::AlbumArtService::decodeToCropped480(jpg.data(), jpg.size(), self->fb_);
          if (ok) { self->blit_row_ = 0; self->ready_ = true; Serial.printf("AlbumArt(bg): background ready (AlbumArtService RAM)\n"); }
          else { Serial.println("AlbumArt(bg): AlbumArtService decode failed"); strncpy(bg_reason, "AlbumArtService decode failed", sizeof(bg_reason)); }
          // Progressive JPEG fallback temporarily disabled for stability
        }
      }

      if (!self->ready_ && self->fb_) {
        // Visible neutral fallback
        for (int y = 0; y < 480; ++y) {
          uint8_t v = (uint8_t)(32 + (y * 192 / 479));
          uint16_t c = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
          for (int x = 0; x < 480; ++x) self->fb_[y*480 + x] = c;
        }
        self->blit_row_ = 0; self->ready_ = true;
        Serial.printf("AlbumArt(bg): fallback gradient shown (reason: %s)\n", bg_reason);
      }

      self->decode_busy_ = false;
      vTaskDelete(NULL);
    };
    const uint32_t stack_words = 24576; // 96KB
    xTaskCreatePinnedToCore(decTask, "aa_decode", stack_words, ctx, tskIDLE_PRIORITY+1, nullptr, 1);
  }
}

void BackgroundArt::blitStep(ui_gfx::Display& disp) {
  // Prefer internal buffer
  if (fb_ && ready_) {
    const int TOP_RESERVE = 80;
    const int BOTTOM_RESERVE = 90 + 40;
    const int STRIP = 24;
    int y_start = blit_row_;
    if (y_start < TOP_RESERVE) y_start = TOP_RESERVE;
    int y_limit = 480 - BOTTOM_RESERVE;
    if (y_start >= y_limit) { blit_row_ = 480; return; }
    int h = (y_start + STRIP <= y_limit) ? STRIP : (y_limit - y_start);
    if (h <= 0) { blit_row_ = 480; return; }
    disp.raw()->draw16bitRGBBitmap(0, y_start, &fb_[(size_t)y_start * 480], 480, h);
    did_blit_ = true;
    blit_row_ = y_start + h;
    return;
  }
  // Fallback to legacy attachment during migration
  if (legacy_fb_ptr_ && *legacy_fb_ptr_ && legacy_ready_ptr_ && *legacy_ready_ptr_) {
    const int TOP_RESERVE = 80;
    const int BOTTOM_RESERVE = 90 + 40;
    const int STRIP = 24;
    int y_start = *legacy_blit_row_ptr_;
    if (y_start < TOP_RESERVE) y_start = TOP_RESERVE;
    int y_limit = 480 - BOTTOM_RESERVE;
    if (y_start >= y_limit) { *legacy_blit_row_ptr_ = 480; return; }
    int h = (y_start + STRIP <= y_limit) ? STRIP : (y_limit - y_start);
    if (h <= 0) { *legacy_blit_row_ptr_ = 480; return; }
    disp.raw()->draw16bitRGBBitmap(0, y_start, &(*legacy_fb_ptr_)[(size_t)y_start * 480], 480, h);
    *legacy_blit_row_ptr_ = y_start + h;
  }
}

bool BackgroundArt::consumeDidBlit() {
  bool v = did_blit_;
  did_blit_ = false;
  return v;
}

void BackgroundArt::blitRegion(ui_gfx::Display& disp, int y0, int y1) {
  if (!(fb_ && ready_)) return;
  if (y0 > y1) return;
  if (y0 < 0) y0 = 0; if (y1 >= 480) y1 = 479;
  if (y0 > y1) return;
  int h = y1 - y0 + 1;
  disp.raw()->draw16bitRGBBitmap(0, y0, &fb_[(size_t)y0 * 480], 480, h);
}

} // namespace albumart

