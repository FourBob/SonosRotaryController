#include <Arduino.h>
#include <esp32-hal-ledc.h>

#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"

#include <WiFi.h>
#include <WiFiUdp.h>


#include <HTTPClient.h>

#include <time.h>
#include "secrets.h"
#include "app_locale.h"
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include <Fonts/FreeSansBold12pt7b.h>
#include <cstring>
#include "esp_heap_caps.h"

#include <Fonts/FreeSansBold12pt7b.h>
#include <PNGdec.h>
#include <TJpg_Decoder.h>
#include <FS.h>
#include <SPIFFS.h>
#include <JPEGDEC.h>
#include <WiFiClientSecure.h>




#include <vector>

#include "assets_speaker_icon_png.h"
#include "sonos.h"
#include "discovery.h"

#include "logging.h"

extern Arduino_RGB_Display *gfx; // forward declaration for png draw callback
static PNG g_png;
static int g_png_draw_x = 0, g_png_draw_y = 0;
static uint16_t g_png_linebuf[480]; // max display width
static uint8_t  g_png_mask[480];    // alpha mask buffer (1 byte per pixel)
static int pngDraw(PNGDRAW *p)
{
  // For icons with alpha: draw with per-pixel mask
  g_png.getLineAsRGB565(p, g_png_linebuf, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
  g_png.getAlphaMask(p, g_png_mask, 200);
  gfx->draw16bitRGBBitmapWithMask(g_png_draw_x, g_png_draw_y + p->y, g_png_linebuf, g_png_mask, p->iWidth, 1);
  return 1;
}

// Album Art (PNG) scaling state and callback
static int g_png_scale = 1;
static int g_png_out_w = 0, g_png_out_h = 0;
static int g_png_center_dx = 0, g_png_center_dy = 0;
static uint16_t g_png_srcbuf[800];      // supports up to 800px wide PNG input lines
static uint16_t g_png_scaled_line[480]; // output line up to display width

static int pngDrawAlbum(PNGDRAW *p)
{
  // Downsample by integer factor g_png_scale and draw centered
  if (g_png_scale < 1) g_png_scale = 1;
  if ((p->y % g_png_scale) != 0) return 1; // skip lines between samples

  // Read the full source line
  g_png.getLineAsRGB565(p, g_png_srcbuf, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);

  // Horizontal downsample
  int out_w = p->iWidth / g_png_scale;
  if (out_w > 480) out_w = 480;
  for (int i = 0; i < out_w; ++i) {
    int sx = i * g_png_scale;
    if (sx >= p->iWidth) sx = p->iWidth - 1;
    g_png_scaled_line[i] = g_png_srcbuf[sx];
  }

  // Compute destination Y (downsampled) and draw
  int y_out = g_png_center_dy + (p->y / g_png_scale);
  if (y_out >= 0 && y_out < 480) {
    gfx->draw16bitRGBBitmap(g_png_center_dx, y_out, g_png_scaled_line, out_w, 1);
  }
  return 1;
}




// I2C (CST826)
#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18

// Backlight and Inputs
#define TFT_BL      38
#define BUTTON_PIN  14
// TJpg_Decoder -> Arduino_GFX bridge
static bool g_albumart_log_verbose = true;
static uint32_t g_tjpg_blk = 0;
static uint32_t g_tjpg_blk_clipped = 0;

static bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  if (w == 0 || h == 0) return true; // skip empty block but continue decode
  if (x >= 480 || y >= 480) return true; // completely off-screen -> continue decode
  uint16_t cw = w, ch = h;
  bool clipped = false;
  if (x + cw > 480) { cw = 480 - x; clipped = true; }
  if (y + ch > 480) { ch = 480 - y; clipped = true; }
  if (cw == 0 || ch == 0) return true; // nothing to draw, but keep going
  // Throttled block logging
  if (g_albumart_log_verbose) {
    uint32_t n = g_tjpg_blk + 1;
    if (n <= 5 || (n % 50) == 0) {
      Serial.printf("AlbumArt: blk#%lu x=%d y=%d w=%u h=%u%s\n",
                    (unsigned long)n, x, y, (unsigned)cw, (unsigned)ch, clipped ? " (clip)" : "");
    }
  }
  if (clipped) g_tjpg_blk_clipped++;
  gfx->draw16bitRGBBitmap(x, y, bitmap, cw, ch);
  g_tjpg_blk++;
  return true;
}

// JPEGDEC -> Arduino_GFX bridge (progressive JPEG fallback)
static int jpegdec_draw_cb(JPEGDRAW *p)
{
  int x = p->x;
  int y = p->y;
  int w = (p->iWidthUsed > 0) ? p->iWidthUsed : p->iWidth;
  int h = p->iHeight;
  if (w <= 0 || h <= 0) return 1;
  if (x >= 480 || y >= 480) return 1;
  if (x + w > 480) w = 480 - x;
  if (y + h > 480) h = 480 - y;
  if (w <= 0 || h <= 0) return 1;
  gfx->draw16bitRGBBitmap(x, y, p->pPixels, w, h);
  return 1;
}


// Album art background job state (must be before task/function use)
static volatile bool g_album_job_busy = false;
static volatile bool g_album_frame_ready = false; // legacy (unused in new path)
static volatile bool g_album_file_ready = false;  // new: file downloaded and ready to decode on main thread
static TaskHandle_t g_album_task = nullptr;

// Non-blocking foreground downloader state (chunked per loop)
static int g_aa_state = 0; // 0=idle, 1=got 200/open file, 2=downloading
static HTTPClient g_aa_http;
static WiFiClientSecure g_aa_cli;
static File g_aa_file;
static size_t g_aa_saved = 0;
static unsigned long g_aa_last_rx_ms = 0;
static unsigned long g_aa_last_log_ms = 0;




// Off-screen framebuffer for JPEGDEC worker output
static uint16_t* g_jpg_fb = nullptr;
static int g_jpg_fb_w = 0, g_jpg_fb_h = 0;

static int jpegdec_draw_to_fb(JPEGDRAW *p)


{
  if (!g_jpg_fb) return 0;
  int x = p->x;
  int y = p->y;
  int src_w = (p->iWidthUsed > 0) ? p->iWidthUsed : p->iWidth;
  int w = src_w;
  int h = p->iHeight;
  if (w <= 0 || h <= 0) return 1;
  int x_off = 0;
  if (x < 0) { x_off = -x; w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= g_jpg_fb_w || y >= g_jpg_fb_h) return 1;
  if (x + w > g_jpg_fb_w) w = g_jpg_fb_w - x;
  if (y + h > g_jpg_fb_h) h = g_jpg_fb_h - y;
  if (w <= 0 || h <= 0) return 1;
  const uint16_t* src = (const uint16_t*)p->pPixels;
  // Copy row-by-row into framebuffer, honoring left clip via x_off and src stride p->iWidth
  for (int row = 0; row < h; ++row) {
    const uint16_t* src_row = src + row * p->iWidth + x_off;
    uint16_t* dst = &g_jpg_fb[(y + row) * g_jpg_fb_w + x];
    memcpy(dst, src_row, (size_t)w * sizeof(uint16_t));
  }
  return 1;
}


// Run JPEGDEC in a worker task to avoid loopTask stack overflow
struct JpegdecTaskCtx {
  const uint8_t* buf;
  int len;
  volatile int result; // 1=ok, 0=fail
  volatile int done;   // 1=worker finished
};

static void jpegdec_worker(void* arg)
{
  JpegdecTaskCtx* ctx = (JpegdecTaskCtx*)arg;
  ctx->result = 0;
  JPEGDEC j;
  ctx->done = 0;
  int jopen = j.openRAM((uint8_t*)ctx->buf, (int)ctx->len, jpegdec_draw_to_fb);
  if (jopen == JPEG_SUCCESS) {
    j.setPixelType(RGB565_LITTLE_ENDIAN);
    int jw = j.getWidth();
    int jh = j.getHeight();
    int jscale = 0; while (((jw >> jscale) > 480 || (jh >> jscale) > 480) && jscale < 3) jscale++;
    if (jscale == 0 && (jw > 320 || jh > 320)) jscale = 1; // force half-scale for >320px to reduce stack/memory
    int out_w = jw >> jscale;
    int out_h = jh >> jscale;
    size_t fb_bytes = (size_t)out_w * (size_t)out_h * sizeof(uint16_t);
    g_jpg_fb = (uint16_t*) heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_jpg_fb) g_jpg_fb = (uint16_t*) heap_caps_malloc(fb_bytes, MALLOC_CAP_8BIT);
    if (!g_jpg_fb) g_jpg_fb = (uint16_t*) malloc(fb_bytes);
    if (!g_jpg_fb) {
      j.close();
      ctx->result = 0;
      ctx->done = 1;
      vTaskDelete(NULL);
      return;
    }
    g_jpg_fb_w = out_w;
    g_jpg_fb_h = out_h;
    int jopts = 0;
    if (jscale == 1) jopts |= JPEG_SCALE_HALF; else if (jscale == 2) jopts |= JPEG_SCALE_QUARTER; else if (jscale == 3) jopts |= JPEG_SCALE_EIGHTH;
    int jrc = j.decode(0, 0, jopts);
    int jerr = j.getLastError();
    j.close();
    if (jrc == JPEG_SUCCESS && jerr == JPEG_SUCCESS) ctx->result = 1;
  }
  ctx->done = 1;
  vTaskDelete(NULL);
}

// Background task: download JPEG and decode to off-screen framebuffer (no scaling)
static void albumart_bg_task(void* arg)
{
  const char* url = "https://i1.sndcdn.com/avatars-NDgaMy6ZFw82pb7S-4Fd2oQ-t500x500.jpg";
  const char* imgPath = "/album.bin";

  // Clean previous FB
  if (g_jpg_fb) { free(g_jpg_fb); g_jpg_fb = nullptr; g_jpg_fb_w = g_jpg_fb_h = 0; }

  HTTPClient http;
  WiFiClientSecure cli;
  cli.setInsecure();
  if (http.begin(cli, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      if (SPIFFS.exists(imgPath)) SPIFFS.remove(imgPath);
      File fout = SPIFFS.open(imgPath, FILE_WRITE);
      if (fout) {
        WiFiClient* stream = http.getStreamPtr();
        const size_t CH = 2048; uint8_t buf[CH];
        unsigned long t0 = millis(), last = t0; size_t written = 0; const unsigned long STALL_MS = 4000; const unsigned long MAX_MS = 20000;
        while (http.connected()) {
          size_t avail = stream->available();
          if (avail) {
            int n = stream->readBytes((char*)buf, (avail > CH) ? CH : avail);
            if (n > 0) { fout.write(buf, n); written += n; last = millis(); }
          } else {
            if (millis() - last > STALL_MS) break;
          }
          if (millis() - t0 > MAX_MS) break;
          delay(1);
        }
        fout.close();
        Serial.printf("AlbumArt(bg): saved %u bytes to %s\n", (unsigned)written, imgPath);
      } else {
        Serial.println("AlbumArt(bg): SPIFFS open write failed");
      }
    } else {
      Serial.printf("AlbumArt(bg): HTTP error %d\n", code);
    }
    http.end();
  }
  // Signal main thread to decode/draw from file
  g_album_file_ready = true;
  g_album_job_busy = false;
  g_album_task = nullptr;
  vTaskDelete(NULL);
}


#define ENCODER_CLK 13
#define ENCODER_DT  10

// Globals
volatile int encoder_counter = 0;
volatile int encoder_state = 0;
volatile int encoder_state_old = 0;
volatile int move_flag = 0;
int button_flag = 0;
int x_touch = 0, y_touch = 0;

// Button ISR state
volatile uint8_t g_btn_raw = 0;            // 0=released, 1=pressed (latched by ISR)
volatile uint8_t g_btn_edge = 0;           // 1 when an edge occurred since last read
volatile unsigned long g_btn_edge_time = 0; // millis() at last edge

// Arduino_GFX v1.3.6/1.3.8+ style initialization
Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 1 /* CS */,
    46 /* SCK */, 0 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    2 /* DE */, 42 /* VSYNC */, 3 /* HSYNC */, 45 /* PCLK */,
    4 /* R0 */, 41 /* R1 */, 5 /* R2 */, 40 /* R3 */, 6 /* R4 */,
    39 /* G0/P22 */, 7 /* G1/P23 */, 47 /* G2/P24 */, 8 /* G3/P25 */, 48 /* G4/P26 */, 9 /* G5 */,
    11 /* B0 */, 15 /* B1 */, 12 /* B2 */, 16 /* B3 */, 21 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type5_init_operations, sizeof(st7701_type5_init_operations));
// --- WiFi / Time helpers --------------------------------------------------
bool g_wifi_ok = false;
// --- Sonos ------------------------------------------------------------------
static SonosClient g_sonos;
static SonosState  g_sonos_state;
static unsigned long g_last_sonos_poll = 0;

unsigned long g_last_time_draw = 0;

// --- Player state -----------------------------------------------------------
static bool     g_playing = true;
static int      g_volume_pct = 20;   // 0..100
static bool     g_muted = false;
static int      g_volume_before_mute = 20;
static int      g_progress_pct = 0;  // 0..100
static String   g_title_line1 = "";
static String   g_title_line2 = "";

// --- Screens / Config menu state ----------------------------------------
enum ScreenMode { SCREEN_PLAYER = 0, SCREEN_CONFIG = 1, SCREEN_CONFIG_BRIGHTNESS = 2, SCREEN_CONFIG_ROOM = 3, SCREEN_CONFIG_ABOUT = 4, SCREEN_CONFIG_ALBUM = 5 };
static ScreenMode g_screen = SCREEN_PLAYER;
static bool     g_config_ui_inited = false;
static int      g_menu_sel = 0;


static bool     g_player_ui_inited = false;
static int      g_last_encoder = 0;
static int      g_enc_accum = 0; // accumulate encoder ticks, apply per 2 ticks (1 detent)
static bool     g_album_ui_inited = false;



// Backlight / Brightness state
static int      g_brightness_pct = 80;   // 0..100
static int      g_bl_last_pct    = -1;   // cache to avoid redundant LEDC writes
static bool     g_brightness_ui_inited = false;

// About screen state
extern bool g_room_ui_inited;

static bool     g_about_ui_inited = false;
static int      g_about_scroll_y = 0;    // vertical offset for scrolling content
static unsigned long g_about_last_scroll = 0;

// Global button state (short vs long press)
static int  g_btn_prev_global = 0;      // 0=released, 1=pressed
static unsigned long g_btn_down_since = 0;
static bool g_btn_long_handled = false; // to fire long action once per hold
static bool g_btn_short_released = false; // edge event: short tap released
static const unsigned long BTN_LONG_MS = 800; // long press threshold (ms)

// LEDC (PWM) config for TFT backlight
static const int BL_CH   = 7;     // use a free LEDC channel
static const int BL_FREQ = 5000;  // Hz
static const int BL_RES  = 8;     // bits (0..255)

static void backlight_set(int pct);
static void backlight_init() {
  // Arduino-ESP32 v3 API: pin-based attach
  ledcAttach(TFT_BL, BL_FREQ, BL_RES);
  backlight_set(g_brightness_pct);
}
static void backlight_set(int pct) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  if (pct == g_bl_last_pct) return;
  int maxd = (1 << BL_RES) - 1;
  int duty = (pct * maxd) / 100;
  ledcWrite(TFT_BL, duty);
  g_bl_last_pct = pct;
}

static const char *wl_status_to_str(wl_status_t s)
{
  switch (s)
  {
    case WL_NO_SHIELD:        return "NO_SHIELD";
    case WL_IDLE_STATUS:      return "IDLE";
    case WL_NO_SSID_AVAIL:    return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:   return "SCAN_COMPLETED";
    case WL_CONNECTED:        return "CONNECTED";
    case WL_CONNECT_FAILED:   return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:  return "CONNECTION_LOST";
    case WL_DISCONNECTED:     return "DISCONNECTED";
    default:                  return "UNKNOWN";
  }
}


static void draw_center_text(const char *msg, uint16_t color = WHITE, uint16_t bg = BLACK, const GFXfont *f = &FreeSansBold24pt7b)
{
  gfx->fillScreen(bg);
  gfx->setTextColor(color, bg);
  gfx->setFont(f);
  gfx->setTextWrap(false);
  int16_t bx, by; uint16_t bw, bh;
  gfx->getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
  int16_t x = (480 - (int)bw) / 2;
  int16_t y = (480 + (int)bh) / 2; // baseline
  gfx->setCursor(x, y);
  gfx->print(msg);
}

// Smooth centered text drawing (clears only previous text area to avoid flicker)
static int16_t g_prev_tlx = 0, g_prev_tly = 0; static uint16_t g_prev_bw = 0, g_prev_bh = 0; static bool g_prev_valid = false;
static void draw_center_text_smooth(const char *msg, uint16_t color = WHITE, const GFXfont *f = &FreeSansBold24pt7b)
{
  gfx->setFont(f);
  gfx->setTextWrap(false);
  int16_t bx, by; uint16_t bw, bh;
  gfx->getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
  int16_t x = (480 - (int)bw) / 2;
  int16_t y = (480 + (int)bh) / 2; // baseline
  int16_t tlx = x + bx;
  int16_t tly = y + by;
  const int pad = 4; // inflate clear area to avoid left-over pixels
  if (!g_prev_valid)
  {
    // First draw: clear just the area where text will be (padded)
    int clrX = max(0, (int)tlx - pad);
    int clrY = max(0, (int)tly - pad);
    int clrW = min(480 - clrX, (int)bw + 2*pad);
    int clrH = min(480 - clrY, (int)bh + 2*pad);
    if (clrW > 0 && clrH > 0) gfx->fillRect(clrX, clrY, clrW, clrH, BLACK);
  }
  else
  {
    // Clear previous text area only (padded)
    int clrX = max(0, (int)g_prev_tlx - pad);
    int clrY = max(0, (int)g_prev_tly - pad);
    int clrW = min(480 - clrX, (int)g_prev_bw + 2*pad);
    int clrH = min(480 - clrY, (int)g_prev_bh + 2*pad);
    if (clrW > 0 && clrH > 0) gfx->fillRect(clrX, clrY, clrW, clrH, BLACK);
  }
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(msg);
  g_prev_tlx = tlx; g_prev_tly = tly; g_prev_bw = bw; g_prev_bh = bh; g_prev_valid = true;
}

// --- Player UI --------------------------------------------------------------
static inline uint16_t RGB(uint8_t r, uint8_t g, uint8_t b){ return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static const int BTN_H = 90;
static const int BTN_W = 160; // 480/3
// Volume icon box (clickable)
// Icon base size (actual x/y computed dynamically for centering)
static const int VOL_ICON_W = 40;
static const int VOL_ICON_H = 40;
static const int VOL_ICON_Y = 12;
// Last drawn hitbox for volume icon (for touch)
static int g_vol_hit_x = 0, g_vol_hit_y = VOL_ICON_Y; static int g_vol_hit_w = VOL_ICON_W, g_vol_hit_h = VOL_ICON_H;
// Progress bar geometry
static const int PRG_X = 40;
static const int PRG_W = 400;
static const int PRG_H = 12;
static const int PRG_Y = 480 - BTN_H - 30;

static void draw_speaker_icon(int x, int y, bool muted)
{
  // Try to draw the exact PNG icon if provided; otherwise fallback to vector icon
  const uint16_t BG = RGB(16,16,16);
  int boxW = VOL_ICON_W, boxH = VOL_ICON_H;

  bool drew_png = false;
  if (speaker_png_len > 0)
  {
    int rc = g_png.openFLASH((uint8_t *)speaker_png, (int)speaker_png_len, pngDraw);
    if (rc == PNG_SUCCESS)
    {
      int pw = g_png.getWidth();
      int ph = g_png.getHeight();
      int scale = 0; // 0=1:1, 1=1/2, 2=1/4, 3=1/8
      while (((pw >> scale) > boxW || (ph >> scale) > boxH) && scale < 3) scale++;
      int sw = max(1, pw >> scale);
      int sh = max(1, ph >> scale);
      g_png_draw_x = x + (boxW - sw) / 2;
      g_png_draw_y = y + (boxH - sh) / 2;
      // Draw PNG with per-pixel alpha via mask (handled in pngDraw)
      g_png.decode(NULL, scale);
      g_png.close();
      drew_png = true;
    }
    else
    {
      g_png.close();
    }
  }

  if (!drew_png)
  {
    // Fallback vector drawing
    int h = boxH - 2; if (h < 12) h = 12;
    int w = (boxW * 7) / 10; if (w < 18) w = 18;
    int bx = x + (boxW - w) / 2;
    int by = y + (boxH - h) / 2;
    const uint16_t FG = WHITE;
    gfx->fillRect(bx, by + 4, max(6, w/4), h - 8, FG);                     // body
    int neckW = max(3, (w/4) / 2);
    gfx->fillRect(bx + max(6, w/4), by + 6, neckW, h - 12, FG);            // neck
    int coneX0 = bx + max(6, w/4) + neckW;
    int coneTipX = bx + w; int coneTopY = by + 2; int coneBotY = by + h - 2; int coneMidY = by + h/2;
    gfx->fillTriangle(coneX0, coneTopY, coneX0, coneBotY, coneTipX, coneMidY, FG);
    if (!muted)
    {
      int cx = coneTipX + max(2, w/16); int cy = coneMidY;
      int r1 = max(6, h/4); int rStep = max(3, h/8); int r2 = r1 + rStep; int r3 = r2 + rStep;
      for (int dr = -1; dr <= 1; ++dr)
      {
        gfx->drawCircle(cx, cy, r1 + dr, FG);
        gfx->drawCircle(cx, cy, r2 + dr, FG);
        gfx->drawCircle(cx, cy, r3 + dr, FG);
      }
      int maskW = r3 + 3;
      gfx->fillRect(cx - maskW, cy - (r3 + 3), maskW, 2*(r3 + 3), BG);
    }
  }

  if (muted)
  {
    // Draw mute slash across the icon box
    for (int i = -2; i <= 2; ++i)
    {
      gfx->drawLine(x - 4, y + i, x + boxW + 4, y + boxH + i, RED);
    }
  }
}

static int g_prev_prog_draw = -1;
static void update_progress_display()
{
  // overlay lane to avoid artifacts from album background
  gfx->fillRect(0, PRG_Y - 10, 480, PRG_H + 20, RGB(10,10,10));
  // background track
  gfx->fillRect(PRG_X, PRG_Y, PRG_W, PRG_H, RGB(30,30,30));
  int fw = (PRG_W * g_progress_pct) / 100;
  gfx->fillRect(PRG_X, PRG_Y, fw, PRG_H, BLUE);
  // knob (thicker)
  int kx = PRG_X + fw; if (kx < PRG_X) kx = PRG_X; if (kx > PRG_X + PRG_W - 1) kx = PRG_X + PRG_W - 1;
  int ky = PRG_Y + PRG_H/2;
  int kr = 9;
  gfx->fillCircle(kx, ky, kr, WHITE);
  // optional border for definition
  gfx->drawRoundRect(PRG_X-1, PRG_Y-1, PRG_W+2, PRG_H+2, 4, RGB(60,60,60));

  // time labels (left: elapsed or LIVE, right: total if known)
  auto fmt_label = [](const String &s)->String {
    int a = s.indexOf(':'); if (a < 0) return String();
    int b = s.indexOf(':', a+1); if (b < 0) return String();
    int h = s.substring(0,a).toInt();
    int m = s.substring(a+1,b).toInt();
    int sec = s.substring(b+1).toInt();
    if (h < 0 || m < 0 || sec < 0) return String();
    char buf[12];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return String(buf);
  };
  String rt = g_sonos_state.relTime;
  String du = g_sonos_state.duration;
  String l = fmt_label(rt);
  String r = fmt_label(du);
  String du_up = du; du_up.trim(); du_up.toUpperCase();
  if (!r.length() || du_up == "NOT_IMPLEMENTED" || du_up == "00:00:00") {
    // If no elapsed time is known, leave it blank (do not show "LIVE")
    r = "";
  }
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  int yText = PRG_Y - 16; // move labels higher to avoid overlap with the bar
  if (l.length()) { gfx->setCursor(PRG_X, yText); gfx->print(l); }
  if (r.length()) {
    int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(r.c_str(), 0, 0, &bx, &by, &bw, &bh);
    int rx = PRG_X + PRG_W - bw; if (rx < PRG_X) rx = PRG_X;
    gfx->setCursor(rx, yText); gfx->print(r);
  }
}

static void draw_icon_play(int cx, int cy, uint16_t col)
{
  int s = 36; // size
  gfx->fillTriangle(cx - s/2, cy - s/2, cx - s/2, cy + s/2, cx + s/2, cy, col);
}
static void draw_icon_pause(int cx, int cy, uint16_t col)
{
  int h = 40, w = 12, gap = 10;
  gfx->fillRect(cx - gap/2 - w, cy - h/2, w, h, col);
  gfx->fillRect(cx + gap/2,     cy - h/2, w, h, col);
}
static void draw_icon_prev(int cx, int cy, uint16_t col)
{
  // Double left-arrow (same size as play)
  int s = 36;      // match play icon size
  int gap = 6;     // small gap between arrows
  int cx_left  = cx - (s/2 + gap/2);
  int cx_right = cx + (s/2 + gap/2);
  // left-pointing triangles
  gfx->fillTriangle(cx_left  + s/2, cy - s/2, cx_left  + s/2, cy + s/2, cx_left  - s/2, cy, col);
  gfx->fillTriangle(cx_right + s/2, cy - s/2, cx_right + s/2, cy + s/2, cx_right - s/2, cy, col);
}
static void draw_icon_next(int cx, int cy, uint16_t col)
{
  // Double right-arrow (same size as play)
  int s = 36;      // match play icon size
  int gap = 6;     // small gap between arrows
  int cx_left  = cx - (s/2 + gap/2);
  int cx_right = cx + (s/2 + gap/2);
  // right-pointing triangles
  gfx->fillTriangle(cx_left  - s/2, cy - s/2, cx_left  - s/2, cy + s/2, cx_left  + s/2, cy, col);
  gfx->fillTriangle(cx_right - s/2, cy - s/2, cx_right - s/2, cy + s/2, cx_right + s/2, cy, col);
}

// Map UTF-8 umlauts and smart punctuation to ASCII fallbacks for GFX fonts
static String ascii_fallback(const String &in)
{
  String out; out.reserve(in.length()+8);
  int n = in.length();
  for (int i = 0; i < n; ) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    // Latin-1 umlauts via 0xC3 prefix
    if (c == 0xC3 && i + 1 < n) {
      uint8_t d = (uint8_t)in[i+1];
      switch (d) {
        case 0x84: out += "Ae"; break; // Ä
        case 0xA4: out += "ae"; break; // ä
        case 0x96: out += "Oe"; break; // Ö
        case 0xB6: out += "oe"; break; // ö
        case 0x9C: out += "Ue"; break; // Ü
        case 0xBC: out += "ue"; break; // ü
        case 0x9F: out += "ss"; break; // ß
        default: /* ignore */ break;
      }
      i += 2; continue;
    }
    // Smart quotes/dashes: E2 80 xx
    if (c == 0xE2 && i + 2 < n && (uint8_t)in[i+1] == 0x80) {
      uint8_t d = (uint8_t)in[i+2];
      switch (d) {
        case 0x98: // ‘
        case 0x99: // ’
          out += "'"; break;
        case 0x9C: // “
        case 0x9D: // ”
          out += '"'; break;
        case 0x93: // – en dash
        case 0x94: // — em dash
          out += '-'; break;
        default:
          break;
      }
      i += 3; continue;
    }
    // Fallback: skip unsupported multi-byte char
    i++;
  }
  return out;
}



// Title/Artist Overlay mit kleinerer Schrift, je 2 Zeilen Wrap
static void draw_title_overlay()
{
  const int maxW = 440; // 20 px Rand links/rechts
  const int y_center = 240;
  const int gap = 6; // Zeilenabstand

  // Title (kleiner Font)
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(WHITE);

  // Hintergrund im Overlay-Bereich dezent löschen, damit sich Zeilen nicht übermalen
  int16_t tbx, tby; uint16_t tbw, tbh; gfx->getTextBounds("Ay", 0, 0, &tbx, &tby, &tbw, &tbh);
  int blockTop = y_center - (int)tbh*2 - 10;
  int blockBot = y_center + (int)tbh*2 + 10;
  if (blockTop < 0) blockTop = 0; if (blockBot > 480) blockBot = 480;
  if (blockBot > blockTop) gfx->fillRect(0, blockTop, 480, blockBot - blockTop, RGB(10,10,10));

  // Manuelles Wrap für Title/Artist (jeweils max 2 Zeilen)
  auto wrap_and_draw = [&](const String &s, int y0) -> int
  {
    // Robust Zweizeilen-Wordwrap ohne Duplikate
    // 1) in Wörter splitten
    std::vector<String> words;
    words.reserve(16);
    int i = 0; while (i < (int)s.length()) {
      while (i < (int)s.length() && s[i] == ' ') i++;
      int j = i; while (j < (int)s.length() && s[j] != ' ') j++;
      if (j > i) words.push_back(s.substring(i, j));
      i = j;
    }
    auto measure = [&](const String &line)->uint16_t{
      int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(line.c_str(), 0, 0, &bx, &by, &bw, &bh); return bw;
    };
    auto draw_line_centered = [&](const String &line, int y){
      int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(line.c_str(), 0, 0, &bx, &by, &bw, &bh);
      int x = (480 - (int)bw) / 2; gfx->setCursor(x, y); gfx->print(line);
      return (int)bh;
    };
    String line1, line2; int idx = 0;
    // Build line1
    while (idx < (int)words.size()) {
      String test = line1.length() ? (line1 + " " + words[idx]) : words[idx];
      if (measure(test) <= maxW) { line1 = test; idx++; }
      else break;
    }
    // Build line2
    while (idx < (int)words.size()) {
      String test = line2.length() ? (line2 + " " + words[idx]) : words[idx];
      if (measure(test) <= maxW) { line2 = test; idx++; }
      else break;
    }
    int y = y0;
    int h1 = 0; if (line1.length()) h1 = draw_line_centered(line1, y);
    int h2 = 0;
    if (line2.length()) {
      y += h1 + gap;
      // Ellipsis, falls Rest übrig
      if (idx < (int)words.size()) {
        String ell = line2 + "...";
        while (measure(ell) > maxW && ell.length() > 3) { ell.remove(ell.length()-4); ell += "..."; }
        h2 = draw_line_centered(ell, y);
      } else {
        h2 = draw_line_centered(line2, y);
      }
    }
    return h1 + (line2.length() ? (gap + h2) : 0);
  };

  int title_y = y_center - 24;  // Start etwas höher
  String t1 = ascii_fallback(g_title_line1);
  String t2 = ascii_fallback(g_title_line2);
  int title_h = wrap_and_draw(t1, title_y);

  // Artist direkt unter dem Title-Block mit zusätzlichem Abstand
  int artist_y = title_y + title_h + 10;
  wrap_and_draw(t2, artist_y);
}

static void draw_player_static()
{

  gfx->fillScreen(RGB(10,10,10));
  // Centered two-line title
  draw_title_overlay();

  // Bottom control bar
  gfx->fillRect(0, 480-BTN_H, 480, BTN_H, RGB(0,0,0));
  // separators
  gfx->drawFastVLine(BTN_W, 480-BTN_H+8, BTN_H-16, RGB(40,40,40));
  gfx->drawFastVLine(BTN_W*2, 480-BTN_H+8, BTN_H-16, RGB(40,40,40));
  // labels (optional)
  // draw transport icons (prev | play/pause | next)
  int y0 = 480 - BTN_H;
  int cy = y0 + BTN_H/2;
  int s = 36; // match play size
  int margin = s/2 + 12; // bring closer to center button
  int cx_prev = BTN_W - margin;       // near right separator of left segment
  int cx_next = BTN_W*2 + margin;     // near left separator of right segment
  draw_icon_prev(cx_prev, cy, WHITE);
  draw_icon_next(cx_next, cy, WHITE);

}

static void update_play_button()
{
  int y0 = 480-BTN_H;
  // middle segment
  gfx->fillRect(BTN_W, y0, BTN_W, BTN_H, RGB(0,0,0));
  int cx = BTN_W + BTN_W/2; int cy = y0 + BTN_H/2;
  if (g_playing) draw_icon_pause(cx, cy, WHITE);
  else           draw_icon_play(cx, cy, WHITE);
}

static int g_prev_vol_draw = -1;
static bool g_prev_muted = false;
static String g_prev_room_drawn = "";

static void update_volume_display()
{
  int effective = g_muted ? 0 : g_volume_pct;
  // Room name to display (ASCII fallback for umlauts)
  String room = ascii_fallback(g_sonos.roomName());

  // Force redraw only if something relevant changed
  if (g_prev_vol_draw == effective && g_prev_muted == g_muted && g_prev_room_drawn == room && g_player_ui_inited) return;
  g_prev_vol_draw = effective; g_prev_muted = g_muted; g_prev_room_drawn = room;

  // Persistent top header band to keep volume visible over album art
  const uint16_t header_bg = RGB(10,10,10);
  int band_y0 = 0;
  int band_h  = 80; // fixed header height
  gfx->fillRect(0, band_y0, 480, band_h, header_bg);

  // Build percent text
  char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%d%%", effective);
  gfx->setFont(&FreeSansBold18pt7b);
  int16_t tbx, tby; uint16_t tbw, tbh;
  gfx->getTextBounds(pbuf, 0, 0, &tbx, &tby, &tbw, &tbh);

  // Total width = icon box + padding + text width
  const int padding = 10;
  int total_w = VOL_ICON_W + padding + (int)tbw;
  int start_x = (480 - total_w) / 2;

  // Click hitbox: enlarged area around icon for easier mute toggle
  g_vol_hit_x = start_x - 60; if (g_vol_hit_x < 0) g_vol_hit_x = 0;
  g_vol_hit_y = VOL_ICON_Y - 12; if (g_vol_hit_y < 0) g_vol_hit_y = 0;
  g_vol_hit_w = VOL_ICON_W + 120; if (g_vol_hit_x + g_vol_hit_w > 480) g_vol_hit_w = 480 - g_vol_hit_x;
  g_vol_hit_h = VOL_ICON_H + 24; if (g_vol_hit_y + g_vol_hit_h > band_h) g_vol_hit_h = band_h - g_vol_hit_y;

  gfx->setTextColor(WHITE, header_bg);

  // Draw icon in its normal box (not the whole hitbox)
  int icon_x = start_x;
  draw_speaker_icon(icon_x, VOL_ICON_Y, g_muted);

  // Draw percentage text next to icon
  int text_x = start_x + VOL_ICON_W + padding;
  int text_base_y = VOL_ICON_Y + (VOL_ICON_H + (int)tbh) / 2 - 2;
  gfx->setCursor(text_x, text_base_y);
  gfx->print(pbuf);

  // Draw room name centered under the volume, using the same font size as the title overlay
  if (room.length()) {
    gfx->setFont(&FreeSansBold12pt7b);
    int16_t rbx, rby; uint16_t rbw, rbh;
    gfx->getTextBounds(room.c_str(), 0, 0, &rbx, &rby, &rbw, &rbh);
    int rx = 240 - (int)rbw / 2;
    int ry = band_y0 + band_h - 10; // a bit above the bottom of the header band
    gfx->setTextColor(WHITE, header_bg);
    gfx->setCursor(rx, ry);
    gfx->print(room);
  }
}

static void player_init()
{
  draw_player_static();
  update_play_button();
  update_volume_display();
  update_progress_display();
  g_player_ui_inited = true;
  g_last_encoder = encoder_counter;
}

static void handle_touch_player(int tx, int ty)
{
  // Volume icon toggle (mute/unmute)
  if (tx >= g_vol_hit_x && tx < g_vol_hit_x + g_vol_hit_w && ty >= g_vol_hit_y && ty < g_vol_hit_y + g_vol_hit_h)
  {
    bool oldMuted = g_muted; int oldVol = g_volume_pct;
    int beforeVol = g_volume_pct;
    if (!g_muted)
    {
      g_muted = true;
      g_volume_before_mute = beforeVol;
    }
    else
    {
      g_muted = false;
      if (g_volume_pct == 0) g_volume_pct = max(5, g_volume_before_mute);
    }
    int effective = g_muted ? 0 : g_volume_pct;
    bool ok = false;
    if (g_sonos.isReady()) ok = g_sonos.setVolume(effective);
    if (!ok) { g_muted = oldMuted; g_volume_pct = oldVol; }
    update_volume_display();
    Serial.printf("Mute: %s %s\n", g_muted ? "on" : "off", ok?"(OK)":"(FAIL)");
    return;
  }

  // Progress bar seek (with some vertical tolerance)
  if (ty >= PRG_Y - 12 && ty <= PRG_Y + PRG_H + 12)
  {
    int nx = tx; if (nx < PRG_X) nx = PRG_X; if (nx > PRG_X + PRG_W - 1) nx = PRG_X + PRG_W - 1;
    int desiredPct = ((nx - PRG_X) * 100) / PRG_W;
    // Compute target time from current duration and send AVTransport.Seek
    auto parse_hms = [](const String &s)->int {
      int a = s.indexOf(':'); if (a < 0) return -1;
      int b = s.indexOf(':', a+1); if (b < 0) return -1;
      int h = s.substring(0,a).toInt();
      int m = s.substring(a+1,b).toInt();
      int sec = s.substring(b+1).toInt();
      if (h < 0 || m < 0 || sec < 0) return -1;
      return h*3600 + m*60 + sec;
    };
    auto fmt_hms = [](int t)->String {
      if (t < 0) t = 0; int h = t/3600; int m = (t/60)%60; int s = t%60;
      char buf[16]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
      return String(buf);
    };
    int durS = parse_hms(g_sonos_state.duration);
    if (durS > 0) {
      int targetS = (desiredPct * durS) / 100;
      String tgt = fmt_hms(targetS);
      bool ok = false;
      if (g_sonos.isReady()) ok = g_sonos.seekRelTime(tgt);
      if (ok) { g_progress_pct = desiredPct; update_progress_display(); }
      else { g_last_sonos_poll = 0; }
      Serial.printf("Seek: %s (%d%%) %s\n", tgt.c_str(), desiredPct, ok?"OK":"FAIL");
    } else {
      Serial.printf("Seek: %d%% (duration unknown)\n", desiredPct);
    }
    return;
  }


  // Bottom transport buttons
  if (ty >= 480 - BTN_H)
  {
    int seg = tx / BTN_W;
    if (seg == 0)
    {
      bool ok = false;
      if (g_sonos.isReady()) ok = g_sonos.previous();
      Serial.printf("Touch: Prev %s\n", ok?"(OK)":"(local)");
    }
    else if (seg == 1)
    {
      bool newPlaying = !g_playing;
      bool ok = false;
      if (g_sonos.isReady()) {
        ok = newPlaying ? g_sonos.play() : g_sonos.pause();
      }
      if (ok) { g_playing = newPlaying; update_play_button(); }
      Serial.printf("Touch: %s %s\n", newPlaying ? L_PLAY : L_PAUSE, ok?"(OK)":"(FAIL)");
    }
    else
    {
      bool ok = false;
      if (g_sonos.isReady()) ok = g_sonos.next();
      Serial.printf("Touch: Next %s\n", ok?"(OK)":"(local)");
    }
    return;
  }
}

static void player_loop()
{
  // Sonos poll: adopt state from selected room
  if (g_sonos.isReady() && millis() - g_last_sonos_poll > 1000)
  {
    g_last_sonos_poll = millis();
    SonosState st = g_sonos_state;
    if (g_sonos.poll(st))
    {
      // Verbose: dump all info read from Sonos
      Serial.printf("Sonos: room=\"%s\" base=%s\n", g_sonos.roomName().c_str(), g_sonos.baseURL().c_str());
      Serial.printf("Sonos: transport=%s playing=%s volume=%d\n",
                    st.transportState.c_str(), st.playing ? "true" : "false", st.volume);
      if (st.title.length() || st.artist.length() || st.album.length()) {
        Serial.printf("Sonos: title=\"%s\" artist=\"%s\" album=\"%s\"\n",
                      st.title.c_str(), st.artist.c_str(), st.album.c_str());
      }
      if (st.relTime.length() || st.duration.length()) {
        Serial.printf("Sonos: time %s / %s\n", st.relTime.c_str(), st.duration.c_str());
      }
      // Play/Pause state
      if (g_playing != st.playing)
      {
        g_playing = st.playing;
        update_play_button();
        Serial.printf("Sonos: state=%s\n", g_playing ? "PLAYING" : "PAUSED");
      }
      // Volume
      if (st.volume >= 0 && g_volume_pct != st.volume)
      {
        g_volume_pct = st.volume;
        g_muted = (g_volume_pct == 0);
        update_volume_display();
        Serial.printf("Sonos: volume=%d\n", g_volume_pct);
      }
      // Title/Artist overlay update
      if (st.title.length() || st.artist.length()) {
        bool tChanged = (g_title_line1 != st.title) || (g_title_line2 != st.artist);
        g_title_line1 = st.title; // keine Fallback-Texte
        g_title_line2 = st.artist;
        if (tChanged) draw_title_overlay();
      } else {
        // No metadata available now: clear any previously shown title/artist
        if (g_title_line1.length() || g_title_line2.length()) {
          g_title_line1 = "";
          g_title_line2 = "";
          draw_title_overlay();
        }
      }
      // Capture time changes before overwriting state
      bool timeChanged = (st.relTime != g_sonos_state.relTime) || (st.duration != g_sonos_state.duration);
      g_sonos_state.transportState = st.transportState;
      g_sonos_state.title          = st.title;
      g_sonos_state.artist         = st.artist;
      g_sonos_state.album          = st.album;
      g_sonos_state.relTime        = st.relTime;
      g_sonos_state.duration       = st.duration;
      g_sonos_state.playing        = st.playing;
      g_sonos_state.volume         = st.volume;
      // Ensure room label under volume updates when room changes
      if (ascii_fallback(g_sonos.roomName()) != g_prev_room_drawn) {
        update_volume_display();
      }
      // Update progress bar from Sonos RelTime/Duration
      {
        auto parse_hms_local = [](const String &s)->int {
          int a = s.indexOf(':'); if (a < 0) return -1;
          int b = s.indexOf(':', a+1); if (b < 0) return -1;
          int h = s.substring(0,a).toInt();
          int m = s.substring(a+1,b).toInt();
          int sec = s.substring(b+1).toInt();
          if (h < 0 || m < 0 || sec < 0) return -1;
          return h*3600 + m*60 + sec;
        };
        int durS = parse_hms_local(st.duration);
        int relS = parse_hms_local(st.relTime);
        if (durS > 0 && relS >= 0) {
          int pct = constrain((relS * 100) / durS, 0, 100);
          if (pct != g_progress_pct) { g_progress_pct = pct; update_progress_display(); timeChanged = false; }
        }
      }
      if (timeChanged) { update_progress_display(); }

    }
      else {
        // Verbose even when no change detected
        Serial.printf("Sonos: room=\"%s\" base=%s\n", g_sonos.roomName().c_str(), g_sonos.baseURL().c_str());
        Serial.printf("Sonos: transport=%s playing=%s volume=%d\n",
                      st.transportState.c_str(), st.playing ? "true" : "false", st.volume);
        if (st.title.length() || st.artist.length() || st.album.length()) {
          Serial.printf("Sonos: title=\"%s\" artist=\"%s\" album=\"%s\"\n",
                        st.title.c_str(), st.artist.c_str(), st.album.c_str());
        }
        if (st.relTime.length() || st.duration.length()) {
          Serial.printf("Sonos: time %s / %s\n", st.relTime.c_str(), st.duration.c_str());
        }
      }

  }

  // Rotary -> volume (apply per 2 ticks to stabilize jitter)
  int cur = encoder_counter;
  int d = cur - g_last_encoder;
  g_last_encoder = cur;
  if (d != 0)
  {
    g_enc_accum += d;
    if (g_enc_accum >= 2 || g_enc_accum <= -2)
    {
      int steps = g_enc_accum / 2; // integer division keeps remainder
      g_enc_accum -= steps * 2;
      int nv = constrain(g_volume_pct + steps, 0, 100);
      bool desiredMuted = (nv == 0);
      int desiredVol = nv;
      bool ok = false;
      if (g_sonos.isReady()) {
        ok = g_sonos.setVolume(desiredMuted ? 0 : desiredVol);
      }
      if (ok) {
        g_muted = desiredMuted;
        g_volume_pct = desiredVol;
        update_volume_display();
      }
      Serial.printf("Volume: %d%s %s\n", g_volume_pct, g_muted?" (muted)":"", ok?"(OK)":"(FAIL)");
    }
  }
  // Touch controls
  int tx, ty;
  if (read_touch(&tx, &ty) == 1)
  {
    handle_touch_player(tx, ty);
  }
  // Button: short tap -> open config menu
  if (g_btn_short_released) {
    g_btn_short_released = false;
    Serial.println("Open Config Menu");
    g_screen = SCREEN_CONFIG;
    g_config_ui_inited = false;
    return;
  }

}


static void show_error(const char *msg)
{
  g_prev_valid = false; // reset smooth text region
  draw_center_text(msg, RED, BLACK);
}

static bool draw_time_now()
{
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return false;
  char buf[20];
  size_t n = strftime(buf, sizeof(buf) - 2, "%H:%M:%S", &ti);
  if (n == 0) {
    // Fallback formatting
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d ", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    buf[n++] = ' ';
    buf[n] = 0;
  }
  // Hard-clear a full-width band where the clock sits to avoid artifacts (e.g. from prior "LIVE")
  gfx->setFont(&FreeSansBold24pt7b);
  int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds("88:88:88", 0, 0, &bx, &by, &bw, &bh);
  int16_t y = (480 + (int)bh) / 2; // baseline similar to draw_center_text_smooth
  int16_t tly = y + by;
  int pad = 6;
  int bandY = max(0, (int)tly - pad);
  int bandH = min(480 - bandY, (int)bh + 2*pad);
  gfx->fillRect(0, bandY, 480, bandH, BLACK);
  g_prev_valid = false; // reset smooth region tracking so we don't rely on previous box
  draw_center_text_smooth(buf, WHITE, &FreeSansBold24pt7b);
  return true;
}

static void connect_wifi_and_time()
{
  draw_center_text("Verbinde WLAN...", WHITE, BLACK);
  Serial.println("WiFi: starting STA...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("SonosRotary");

  Serial.printf("WiFi: begin SSID=\"%s\"\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  wl_status_t st = WiFi.status();
  while (st != WL_CONNECTED && millis() - start < 25000)
  {
    delay(250);
    wl_status_t ns = WiFi.status();
    if (ns != st)
    {
      st = ns;
      Serial.printf("WiFi status: %s\n", wl_status_to_str(st));
    }
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    g_wifi_ok = false;
    Serial.printf("WiFi failed: status=%s\n", wl_status_to_str(WiFi.status()));

    int n = WiFi.scanNetworks();
    Serial.printf("Scan found %d networks:\n", n);
    for (int i = 0; i < n; ++i)
    {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      auto enc = WiFi.encryptionType(i);
      Serial.printf("  %s (RSSI %d) %s\n", ssid.c_str(), rssi, (enc == WIFI_AUTH_OPEN ? "[open]" : ""));
    }

    show_error("WLAN fehlgeschlagen");
    return;
  }

  g_wifi_ok = true;
  g_prev_valid = false; // first time draw should start clean
  Serial.printf("WiFi connected: IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  bool time_ok = false;
  for (int i = 0; i < 6; ++i)
  {
    if (draw_time_now()) { g_last_time_draw = millis(); time_ok = true; break; }
    delay(400);
  }
  if (!time_ok) {
    show_error("Zeitserver Fehler");
  }

  // Sonos connect is handled in setup() using DEFAULT_SONOS_ROOM (non-blocking).
  // Avoid blocking here to keep UI responsive on boot.
}


void IRAM_ATTR encoder_irq()
{
  int s = digitalRead(ENCODER_CLK);
  if (s != encoder_state_old)
  {

    if (digitalRead(ENCODER_DT) == s)
      encoder_counter++;
    else
      encoder_counter--;
    encoder_state_old = s;
    move_flag = 1;
  }
}

void draw_page()
{
  static uint16_t colors[] = {WHITE, BLUE, GREEN, RED, YELLOW};
  uint16_t c = colors[(abs(encoder_counter) / 2) % (sizeof(colors)/sizeof(colors[0]))];
  gfx->fillScreen(c);
  gfx->setTextSize(3);
  gfx->setTextColor(BLACK);

  gfx->setCursor(120, 100);
  gfx->println(F("Makerfabs"));

  gfx->setTextSize(2);
  gfx->setCursor(40, 160);
  gfx->println(F("2.1\" ST7701S RGB + CST826 Touch"));

  gfx->setTextSize(3);
  gfx->setCursor(60, 200);
  gfx->print(F("Encoder: "));
  gfx->println(encoder_counter);

  gfx->setCursor(60, 240);
  gfx->print(F("BUTTON: "));
  gfx->println(button_flag);

  gfx->setCursor(60, 280);
  gfx->print(F("Touch X: "));
  gfx->println(x_touch);

  gfx->setCursor(60, 320);
  gfx->print(F("Touch Y: "));
  gfx->println(y_touch);
}


void IRAM_ATTR button_isr()
{
  uint8_t b = (digitalRead(BUTTON_PIN) == LOW) ? 1 : 0;
  g_btn_raw = b;
  g_btn_edge = 1;
  g_btn_edge_time = millis();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("MaTouch ESP32-S3 2.1\" init");

  backlight_init();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  g_btn_raw = (digitalRead(BUTTON_PIN) == LOW) ? 1 : 0;
  g_btn_prev_global = g_btn_raw;

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  encoder_state_old = digitalRead(ENCODER_CLK);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Init Display
  // Init SPIFFS for file-based image decoding
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS: mount failed");
  } else {
    Serial.println("SPIFFS: mounted");
  }
  gfx->begin();
  connect_wifi_and_time();

  // Try to connect to default Sonos room (use DiscoveryManager fast-path, fallback to legacy discover)
  #ifdef DEFAULT_SONOS_ROOM
  if (!g_sonos.isReady()) {
    String def = String(DEFAULT_SONOS_ROOM);
    if (def.length()) {
      Serial.printf("Sonos: default room connect: %s\n", def.c_str());

      bool ok = false;
      // 1) Use centralized discovery to gather Sonos players quickly (filtered, non-blocking during window)
      {
        DiscoveryManager& dm = DiscoveryManager::instance();
        dm.mergeBurst(2000); // short burst to populate cache
        std::vector<RoomInfo> rooms; dm.getRooms(rooms);
        String base;
        for (auto &ri : rooms) { if (ri.name.equalsIgnoreCase(def)) { base = ri.base; break; } }
        if (base.length()) {
          Serial.printf("Sonos: default using cached base for \"%s\": %s\n", def.c_str(), base.c_str());
          ok = g_sonos.connectKnown(base, def);
        }
      }
      // 2) Fallback: legacy SSDP-based discover with slightly extended timeout
      if (!ok) {
        ok = g_sonos.discoverRoom(def, 2800);
      }

      if (ok) {
        Serial.printf("Sonos: default connected room=\"%s\" base=%s\n", g_sonos.roomName().c_str(), g_sonos.baseURL().c_str());
        // schedule fresh poll; let player do it without blocking setup
        g_player_ui_inited = false;
        g_last_sonos_poll = 0;
        // Clear any stale UI metadata until the first poll provides fresh data
        g_title_line1 = ""; g_title_line2 = ""; g_progress_pct = 0;
        g_sonos_state.relTime = ""; g_sonos_state.duration = ""; g_sonos_state.transportState = "";
      } else {
        Serial.println("Sonos: default connect failed");
      }
    }
  }
  #endif

  Serial.println("setup: attaching interrupts");
  attachInterrupt(BUTTON_PIN, button_isr, CHANGE);
  attachInterrupt(ENCODER_CLK, encoder_irq, CHANGE);
}


static void draw_config_menu_list()
{
  // Only (re)draw list items area to avoid full-screen flicker
  const int N = 7;
  const int y0_base = 120; const int dy = 48;
  gfx->setFont(&FreeSansBold12pt7b);
  for (int i = 0, y0 = y0_base; i < N; ++i, y0 += dy) {
    bool sel = (i == g_menu_sel);
    uint16_t bg = sel ? RGB(60,60,90) : RGB(10,10,10);
    uint16_t fg = sel ? WHITE : RGB(210,210,210);
    // clear row area and draw background/highlight bar
    gfx->fillRect(30, y0 - 30, 420, 44, RGB(10,10,10));
    if (sel) gfx->fillRoundRect(40, y0 - 26, 400, 40, 10, bg);
    // labels
    String labelStr;
    if (i == 0) labelStr = String(L_BRIGHTNESS);
    else if (i == 1) labelStr = String(L_SONOS_ROOM);
    else if (i == 2) labelStr = String(L_ALBUM_ART);
    else if (i == 3) labelStr = String(L_ABOUT);
    else if (i == 4) labelStr = String(L_ALBUM_ART_LOG) + String(": ") + (g_albumart_log_verbose ? String("An") : String("Aus"));
    else if (i == 5) labelStr = String("Reset");
    else if (i == 6) labelStr = String("Zurueck zum Player");
    String label = ascii_fallback(labelStr);
    int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(label.c_str(), 0, 0, &bx, &by, &bw, &bh);
    int x = (480 - (int)bw) / 2; int y = y0;
    gfx->setTextColor(fg, sel ? bg : RGB(10,10,10));
    gfx->setCursor(x, y);
    gfx->print(label);
  }
}

static void draw_config_static()
{
  // Background
  gfx->fillScreen(RGB(10,10,10));
  // Title
  gfx->setFont(&FreeSansBold18pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  gfx->setCursor(120, 60);
  gfx->print(ascii_fallback(String("Einstellungen")));
  // Menu items
  draw_config_menu_list();
}

static void config_loop()
{
  if (!g_config_ui_inited) {
    g_config_ui_inited = true;
    g_menu_sel = 0;
    g_last_encoder = encoder_counter;
    draw_config_static();
  }
  // Encoder navigation (less sensitive: 2 ticks per step)
  static int cfg_enc_accum = 0;
  int cur = encoder_counter;
  int d = cur - g_last_encoder;
  g_last_encoder = cur;
  if (d != 0) {
    cfg_enc_accum += d;
    if (cfg_enc_accum >= 2 || cfg_enc_accum <= -2) {
      int steps = cfg_enc_accum / 2; // integer division keeps remainder
      cfg_enc_accum -= steps * 2;
      g_menu_sel += steps;
      const int N = 7;
      while (g_menu_sel < 0) g_menu_sel += N;
      while (g_menu_sel >= N) g_menu_sel -= N;
      draw_config_menu_list();
    }
  }
  // Button: short tap -> select menu item
  if (g_btn_short_released) {
    g_btn_short_released = false;
    Serial.printf("Config: select item %d\n", g_menu_sel);
    if (g_menu_sel == 0) {
      g_screen = SCREEN_CONFIG_BRIGHTNESS;
      g_brightness_ui_inited = false;
    } else if (g_menu_sel == 1) {
      g_screen = SCREEN_CONFIG_ROOM;
      g_room_ui_inited = false;
    } else if (g_menu_sel == 2) {
      g_screen = SCREEN_CONFIG_ALBUM;
      g_album_ui_inited = false;
    } else if (g_menu_sel == 3) {
      g_screen = SCREEN_CONFIG_ABOUT;
      g_about_ui_inited = false;
    } else if (g_menu_sel == 4) {
      // Toggle Album-Art verbose logging
      g_albumart_log_verbose = !g_albumart_log_verbose;
      Serial.printf("AlbumArt: verbose logging %s\n", g_albumart_log_verbose ? "ON" : "OFF");
      draw_config_menu_list();
    } else if (g_menu_sel == 5) {
      // Reset device (reboot)
      Serial.println("Config: Reset -> ESP.restart()");
      delay(50);
      ESP.restart();
    } else if (g_menu_sel == 6) {
      // Back to player
      g_screen = SCREEN_PLAYER;
      g_player_ui_inited = false;
      g_config_ui_inited = false;
      return;
    } else {
      // Fallback: return to player
      g_screen = SCREEN_PLAYER;
      g_player_ui_inited = false;
    }
    return;
  }
}

// --- Config Subpages ---------------------------------------------------------
// Brightness UI
static void draw_brightness_slider()
{
  // Slider geometry
  const int X = 60, Y = 240, W = 360, H = 16;
  // Clear slider area
  gfx->fillRect(X-10, Y-20, W+20, 60, RGB(10,10,10));
  // Track
  gfx->fillRoundRect(X, Y, W, H, 8, RGB(40,40,40));
  int fw = (W * g_brightness_pct) / 100;
  if (fw < 0) fw = 0; if (fw > W) fw = W;
  gfx->fillRoundRect(X, Y, fw, H, 8, RGB(120,160,255));
  // Knob
  int kx = X + fw; int ky = Y + H/2; int kr = 12;
  gfx->fillCircle(kx, ky, kr, WHITE);
  gfx->fillCircle(kx, ky, kr-4, RGB(60,60,60));
  // Percent label
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  char buf[16]; snprintf(buf, sizeof(buf), "%d%%", g_brightness_pct);
  int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  gfx->setCursor(240 - (int)bw/2, Y + 40);
  gfx->print(buf);
}
static void draw_brightness_static()
{
  gfx->fillScreen(RGB(10,10,10));
  // Title
  gfx->setFont(&FreeSansBold18pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  gfx->setCursor(120, 60);
  gfx->print(ascii_fallback(String(L_BRIGHTNESS)));
  // Hint
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(RGB(200,200,200), RGB(10,10,10));
  gfx->setCursor(60, 120);
  gfx->print(ascii_fallback(String("Drehrad: aendern, Taste: zurueck")));
  draw_brightness_slider();
}
static void brightness_loop()
{
  if (!g_brightness_ui_inited) {
    g_brightness_ui_inited = true;
    g_last_encoder = encoder_counter;
    draw_brightness_static();
  }
  // Encoder: 2 ticks -> 1% Schritt
  static int bl_acc = 0;
  int cur = encoder_counter; int d = cur - g_last_encoder; g_last_encoder = cur;
  if (d != 0) {
    bl_acc += d;
    if (bl_acc >= 2 || bl_acc <= -2) {
      int steps = bl_acc / 2; bl_acc -= steps * 2;
      int np = g_brightness_pct + steps;
      if (np < 0) np = 0; if (np > 100) np = 100;
      if (np != g_brightness_pct) {
        g_brightness_pct = np;
        backlight_set(g_brightness_pct);
        draw_brightness_slider();
      }
    }
  }
  // Touch: tap on slider track to set brightness
  {
    int tx, ty;
    if (read_touch(&tx, &ty) == 1) {
      const int X = 60, Y = 240, W = 360, H = 16;
      if (tx >= X-10 && tx <= X+W+10 && ty >= Y-20 && ty <= Y+40) {
        int rel = tx - X; if (rel < 0) rel = 0; if (rel > W) rel = W;
        int np = (rel * 100) / W;
        if (np != g_brightness_pct) {
          g_brightness_pct = np;
          backlight_set(g_brightness_pct);
          draw_brightness_slider();
        }
      }
    }
  }
  // Button: back to Config (short tap)
  if (g_btn_short_released) {
    g_btn_short_released = false;
    // After setting brightness, return to Player
    g_screen = SCREEN_PLAYER;
    g_player_ui_inited = false;
    g_config_ui_inited = false;
    return;
  }
}

// --- Sonos Room Select UI --------------------------------------------------
bool   g_room_ui_inited = false;
static int    g_room_sel = 0;
static String g_room_names[16];
static String g_room_bases[16]; // base URL per room if known from SSDP (e.g. http://ip:1400)
static int    g_room_count = 0;
static volatile bool g_discovery_paused = false; // pause SSDP scans during connect

static unsigned long g_room_last_bg_scan = 0;
static const uint32_t ROOM_BG_SCAN_INTERVAL_MS = 30000; // 30s
static unsigned long g_room_last_ui_scan = 0; // periodic scan while in room UI


static void scan_sonos_rooms(uint32_t timeout_ms = 2000, bool merge = false, bool light = false)
{
  if (g_discovery_paused) { LOGD("Rooms: scan paused\n"); return; }

  // Centralized discovery: short SSDP burst via DiscoveryManager
  DiscoveryManager& dm = DiscoveryManager::instance();
  dm.mergeBurst(timeout_ms);

  // Mirror cache into legacy arrays for UI rendering
  std::vector<RoomInfo> rooms; dm.getRooms(rooms);
  g_room_count = 0;
  for (auto &ri : rooms) {
    if (g_room_count >= (int)(sizeof(g_room_names)/sizeof(g_room_names[0]))) break;
    g_room_names[g_room_count] = ri.name;
    g_room_bases[g_room_count] = ri.base;
    g_room_count++;
  }

  // Minimal fallback: if nothing via SSDP and we have a current base, ask /status/topology
  if (!light && g_room_count == 0 && g_sonos.isReady()) {
    String base = g_sonos.baseURL();
    if (base.length()) {
      HTTPClient http;
      String url2 = base + "/status/topology";
      if (http.begin(url2)) {
        http.setTimeout(1200);
        http.addHeader("Connection", "close");
        int code2 = http.GET();
        Serial.printf("TOPO: http=%d\n", code2);
        if (code2 == HTTP_CODE_OK) {
          String topo = http.getString();
          int pos2 = 0;
          while (pos2 >= 0 && g_room_count < (int)(sizeof(g_room_names)/sizeof(g_room_names[0]))) {
            int np2 = topo.indexOf("zoneName=\"", pos2);
            int keylen2 = 10;
            if (np2 < 0) { np2 = topo.indexOf("ZoneName=\"", pos2); keylen2 = 10; }
            if (np2 < 0) break;
            np2 += keylen2; int ne2 = topo.indexOf('\"', np2);
            if (ne2 < 0) break;
            String name2 = topo.substring(np2, ne2); name2.trim();
            bool exists2 = false; for (int i=0;i<g_room_count;i++) if (g_room_names[i].equalsIgnoreCase(name2)) { exists2 = true; break; }
            if (!exists2 && name2.length()) g_room_names[g_room_count++] = name2;
            pos2 = ne2 + 1;
            if (!exists2 && name2.length()) Serial.printf("TOPO: room=\"%s\"\n", name2.c_str());
          }
        }
        http.end();
      }
    }
  }

  LOGD("Rooms: total %d\n", g_room_count);
}

static void draw_room_list()
{
  const int y0_base = 140; const int dy = 44;
  gfx->setFont(&FreeSansBold12pt7b);
  for (int i = 0, y0 = y0_base; i < max(g_room_count, 1); ++i, y0 += dy) {
    bool sel = (i == g_room_sel);
    uint16_t bg = sel ? RGB(60,60,90) : RGB(10,10,10);
    uint16_t fg = sel ? WHITE : RGB(210,210,210);
    // clear row area
    gfx->fillRect(30, y0 - 28, 420, 40, RGB(10,10,10));
    if (sel) gfx->fillRoundRect(40, y0 - 24, 400, 36, 10, bg);
    String label = (g_room_count>0) ? g_room_names[i] : String("Suche...");
    label = ascii_fallback(label);
    int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds(label.c_str(), 0, 0, &bx, &by, &bw, &bh);
    gfx->setTextColor(fg, sel?bg:RGB(10,10,10));
    gfx->setCursor(240 - (int)bw/2, y0);
    gfx->print(label);
  }
}

static void draw_room_static()
{
  gfx->fillScreen(RGB(10,10,10));
  gfx->setFont(&FreeSansBold18pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  gfx->setCursor(120, 60);
  gfx->print(ascii_fallback(String(L_SONOS_ROOM)));
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(RGB(200,200,200), RGB(10,10,10));
  gfx->setCursor(60, 110);
  gfx->print(ascii_fallback(String("Drehrad: waehlen, Taste: setzen")));
  draw_room_list();
}

static void room_loop()
{
  if (!g_room_ui_inited) {
    g_room_ui_inited = true;
    g_last_encoder = encoder_counter;
    // first: one heavy query to fetch full list quickly
    scan_sonos_rooms(1200, true, false);
    // preselect current room if present
    String cur = g_sonos.roomName();
    g_room_sel = 0; for (int i=0;i<g_room_count;i++) if (g_room_names[i].equalsIgnoreCase(cur)) { g_room_sel = i; break; }
    draw_room_static();
  }
  // Encoder navigation (2 ticks per step), wrap
  static int acc = 0;
  int curEnc = encoder_counter; int d = curEnc - g_last_encoder; g_last_encoder = curEnc;
  if (d != 0 && g_room_count>0) {
    acc += d;
    if (acc >= 2 || acc <= -2) {
      int steps = acc / 2; acc -= steps * 2;
      g_room_sel += steps;
      while (g_room_sel < 0) g_room_sel += g_room_count;
      while (g_room_sel >= g_room_count) g_room_sel -= g_room_count;
      draw_room_list();
    }
  }
  // Periodic UI room scan while this page is open (merge results, LIGHT to avoid blocking UI)
  if (millis() - g_room_last_ui_scan >= 1200) {
    g_room_last_ui_scan = millis();
    int before = g_room_count;
    scan_sonos_rooms(200, true, true); // short, SSDP-only
    if (g_room_count != before) {
      // keep selection in range and refresh list
      if (g_room_count > 0) {
        while (g_room_sel < 0) g_room_sel += g_room_count;
        while (g_room_sel >= g_room_count) g_room_sel -= g_room_count;
      } else {
        g_room_sel = 0;
      }
      draw_room_list();
    }
  }

  // Touch: tap a row -> select immediately
  int tx, ty;
  if (read_touch(&tx, &ty) == 1 && g_room_count>0) {
    const int y0_base = 140; const int dy = 44;
    for (int i=0, y0=y0_base; i<g_room_count; ++i, y0+=dy) {
      if (ty >= y0-28 && ty <= y0+12) { g_room_sel = i; draw_room_list(); break; }
    }
  }
  // Button: set selected room (discover) on short tap
  if (g_btn_short_released && g_room_count > 0) {
    g_btn_short_released = false;
    String sel = g_room_names[g_room_sel];
    gfx->setFont(&FreeSansBold12pt7b);
    gfx->setTextColor(WHITE, RGB(10,10,10));
    gfx->fillRect(80, 400, 320, 40, RGB(10,10,10));
    gfx->setCursor(120, 430);
    gfx->print(ascii_fallback(String("Verbinde: ")+sel+String("...")));

    // Pause discovery while connecting to avoid UDP contention
    g_discovery_paused = true;
    bool ok = false;
    {
      String base = g_room_bases[g_room_sel];
      if (base.length()) {
        LOGI("Rooms: using cached base for \"%s\": %s\n", sel.c_str(), base.c_str());
        ok = g_sonos.connectKnown(base, sel);
      } else {
        ok = g_sonos.discoverRoom(sel, 1200);
      }
    }
    g_discovery_paused = false;

    if (ok) {
      LOGI("Rooms: connected to \"%s\" -> %s\n", g_sonos.roomName().c_str(), g_sonos.baseURL().c_str());
      // Switch to Player immediately; schedule poll for player loop (no blocking here)
      g_screen = SCREEN_PLAYER;
      g_player_ui_inited = false;
      g_config_ui_inited = false;
      g_room_ui_inited = false;
      g_title_line1 = ""; g_title_line2 = "";
      g_progress_pct = 0;
      g_last_sonos_poll = 0;
      g_sonos_state.relTime = "";
      g_sonos_state.duration = "";
      g_sonos_state.transportState = "";
      return;
    } else {
      // stay on room screen to let the user try again
      g_screen = SCREEN_CONFIG_ROOM;
      g_room_ui_inited = false;
    }
    return;
}
}


// About UI
static const char* ABOUT_LINES[] = {
  "Sonos Rotary Controller",
  "Drehrad: Lautstaerke & Menue",
  "Touch: Play/Pause, Prev/Next",

  "Config: Helligkeit, Raum, Timeout",
  "Open Source – siehe README"
};
static void draw_about_static()
{
  gfx->fillScreen(RGB(10,10,10));
  // Title
  gfx->setFont(&FreeSansBold18pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  gfx->setCursor(120, 60);
  gfx->print(ascii_fallback(String("Ueber")));
  // Start scroll just below title
  g_about_scroll_y = 140;
  g_about_last_scroll = millis();
}
static void draw_about_content()
{
  // Content area clear
  gfx->fillRect(0, 90, 480, 360, RGB(10,10,10));
  gfx->setFont(&FreeSansBold12pt7b);
  gfx->setTextColor(WHITE, RGB(10,10,10));
  int16_t bx, by; uint16_t bw, bh; gfx->getTextBounds("Ay", 0, 0, &bx, &by, &bw, &bh);
  int lh = (int)bh + 6; // line height
  for (int i = 0; i < (int)(sizeof(ABOUT_LINES)/sizeof(ABOUT_LINES[0])); ++i) {
    String line = ascii_fallback(String(ABOUT_LINES[i]));
    gfx->getTextBounds(line.c_str(), 0, 0, &bx, &by, &bw, &bh);
    int x = (480 - (int)bw)/2;
    int y = g_about_scroll_y + i*lh;
    if (y > 100 && y < 460) {
      gfx->setCursor(x, y);
      gfx->print(line);
    }
  }
}
static void about_loop()
{
  if (!g_about_ui_inited) {
    g_about_ui_inited = true;
    draw_about_static();
    draw_about_content();
  }
  // Auto-scroll


  // No auto-scroll; keep content static on the screen
  // (Content is drawn once during init in draw_about_content())
  // Button: back to Config (short tap)
  if (g_btn_short_released) {
    g_btn_short_released = false;
    g_screen = SCREEN_CONFIG;
    g_config_ui_inited = false;
    return;
  }
}


#if 0

static void albumart_loop()
{
  if (!g_album_ui_inited) {
    g_album_ui_inited = true;
    gfx->fillScreen(BLACK);
    draw_center_text("Lade Album Art...", WHITE, BLACK);

    // Test: fixed URL for Albumart screen
    String url = String("https://i1.sndcdn.com/avatars-NDgaMy6ZFw82pb7S-4Fd2oQ-t500x500.jpg");
    Serial.println("AlbumArt: using test URL (SoundCloud avatar 500x500)");

    if (url.length()) {
      // JPEG output color order: for Arduino_GFX draw16bitRGBBitmap we do NOT swap bytes
      TJpgDec.setSwapBytes(false);

      TJpgDec.setCallback(tjpg_output);

      HTTPClient http;
      WiFiClientSecure httpsClient;
      httpsClient.setInsecure(); // accept default CA for demo/test
      http.begin(httpsClient, url);
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String ctype = http.header("Content-Type");
        WiFiClient *stream = http.getStreamPtr();
        int total = http.getSize();
        Serial.printf("AlbumArt: HTTP %d, type=%s, size=%d\n", code, ctype.c_str(), total);
        std::vector<uint8_t> jpg;
        if (total > 0 && total < 800000) jpg.reserve(total);
        const size_t CHUNK = 2048;
        uint8_t buf[CHUNK];
        unsigned long t0 = millis();
        unsigned long lastProg = t0;
        size_t lastBytes = 0;
        const unsigned long STALL_MS = 4000;  // timeout since last progress
        const unsigned long MAX_MS   = 20000; // absolute cap
        while (http.connected()) {
          size_t avail = stream->available();
          if (avail) {
            int n = stream->readBytes((char*)buf, (avail > CHUNK) ? CHUNK : avail);
            if (n > 0) { jpg.insert(jpg.end(), buf, buf + n); lastProg = millis(); lastBytes += n; }
          } else {
            if (total > 0 && (int)jpg.size() >= total) break; // finished
            if (millis() - lastProg > STALL_MS) break;        // stalled
          }
          if (millis() - t0 > MAX_MS) break;                  // absolute timeout
          delay(1);
        }
        if (total > 0) Serial.printf("AlbumArt: recv %d/%d bytes\n", (int)jpg.size(), total);
        else Serial.printf("AlbumArt: recv %d bytes (chunked)\n", (int)jpg.size());
        if (!jpg.empty()) {
          // Prefer robust path: decode JPEGs with JPEGDEC worker to avoid partial/progressive issues
          bool is_png = (jpg.size() >= 8 && jpg[0]==0x89 && jpg[1]==0x50 && jpg[2]==0x4E && jpg[3]==0x47 && jpg[4]==0x0D && jpg[5]==0x0A && jpg[6]==0x1A && jpg[7]==0x0A);
          if (!is_png) {
            JpegdecTaskCtx ctx; ctx.buf = jpg.data(); ctx.len = (int)jpg.size(); ctx.result = 0;
            unsigned long jt0 = millis();
            TaskHandle_t h = nullptr;
            // Use a larger stack (8192 words = 32KB) on core 0
            xTaskCreatePinnedToCore(jpegdec_worker, "jpegdec", 12288, &ctx, tskIDLE_PRIORITY+1, &h, 0);
            // Wait up to 7 seconds for completion
            while (!ctx.done && (millis() - jt0) < 7000) {
              delay(10);
            }
            if (ctx.result == 1 && g_jpg_fb) {
              int fbw = g_jpg_fb_w, fbh = g_jpg_fb_h;
              int dx2 = (480 - fbw) / 2; if (dx2 < 0) dx2 = 0;
              int dy2 = (480 - fbh) / 2; if (dy2 < 0) dy2 = 0;
              gfx->fillScreen(BLACK);
              for (int row = 0; row < fbh; ++row) {
                gfx->draw16bitRGBBitmap(dx2, dy2 + row, &g_jpg_fb[row * fbw], fbw, 1);
              }
              Serial.printf("AlbumArt: JPEGDEC drawn ok (forced), src=%dx%d, dst at (%d,%d)\n", fbw, fbh, dx2, dy2);
              free(g_jpg_fb); g_jpg_fb = nullptr; g_jpg_fb_w = g_jpg_fb_h = 0;
            } else {
              Serial.println("AlbumArt: JPEGDEC decode failed (forced)");
              draw_center_text("Kein Album Art", WHITE, BLACK);
            }
        // Robust path: stream to SPIFFS, then read back and decode with TJpg_Decoder (baseline JPEG)
        const char *imgPath = "/album.bin";
        if (SPIFFS.exists(imgPath)) SPIFFS.remove(imgPath);
        File fout = SPIFFS.open(imgPath, FILE_WRITE);
        if (!fout) { Serial.println("AlbumArt: SPIFFS open write failed"); http.end(); draw_center_text("Kein Album Art", WHITE, BLACK); return; }
        {
          const size_t CHUNK2 = 2048; uint8_t buf2[CHUNK2];
          unsigned long t0 = millis(); size_t written = 0; unsigned long lastProg = t0; const unsigned long STALL_MS = 4000;
          while (http.connected()) {
            size_t avail = stream->available();
            if (avail) { int n = stream->readBytes((char*)buf2, (avail > CHUNK2) ? CHUNK2 : avail); if (n > 0) { fout.write(buf2, n); written += n; lastProg = millis(); } }
            else { if (millis() - lastProg > STALL_MS) break; }
            if (millis() - t0 > 20000) break;
            delay(1);
          }
          fout.close();
          Serial.printf("AlbumArt: saved %u bytes to %s\n", (unsigned)written, imgPath);
        }
        // Read back into RAM for TJpgDec (keeps code simple and stable)
        std::vector<uint8_t> jpg;
        {
          File fin = SPIFFS.open(imgPath, FILE_READ);
          if (fin) {
            size_t sz = fin.size(); if (sz > 0 && sz < 800000) jpg.reserve(sz);
            const size_t CHUNK2 = 2048; uint8_t buf2[CHUNK2];
            while (fin.available()) { int n = fin.read(buf2, CHUNK2); if (n <= 0) break; jpg.insert(jpg.end(), buf2, buf2 + n); }
            fin.close();
          }
        }
        if (jpg.empty()) { http.end(); draw_center_text("Kein Album Art", WHITE, BLACK); return; }
        uint16_t w=0, hpx=0; int scale=0; int dx=0, dy=0;
        if (TJpgDec.getJpgSize(&w, &hpx, jpg.data(), jpg.size()) == JDR_OK) {
          while (((w >> scale) > 480 || (hpx >> scale) > 480) && scale < 3) scale++;
          dx = (480 - (w >> scale)) / 2; if (dx < 0) dx = 0;
          dy = (480 - (hpx >> scale)) / 2; if (dy < 0) dy = 0;
        }
        TJpgDec.setJpgScale(scale);
        gfx->fillScreen(BLACK);
        bool ok = TJpgDec.drawJpg(dx, dy, jpg.data(), jpg.size());
        if (ok) Serial.printf("AlbumArt: drawn ok (file->RAM), jpg=%ux%u, scale=%d at (%d,%d)\n", w, hpx, scale, dx, dy);
        else Serial.println("AlbumArt: drawJpg failed");
        http.end();
        return;

        }

#if 0
          uint16_t w = 0, h = 0;
          if (TJpgDec.getJpgSize(&w, &h, jpg.data(), jpg.size()) == JDR_OK) {
            int scale = 0;
            while (((w >> scale) > 480 || (h >> scale) > 480) && scale < 3) scale++;
            TJpgDec.setJpgScale(scale);
            int dx = (480 - (w >> scale)) / 2; if (dx < 0) dx = 0;
            int dy = (480 - (h >> scale)) / 2; if (dy < 0) dy = 0;
            gfx->fillScreen(BLACK);
            g_tjpg_blk = 0; g_tjpg_blk_clipped = 0;
            unsigned long tdec0 = millis();
            bool ok = TJpgDec.drawJpg(dx, dy, jpg.data(), jpg.size());
            unsigned long tdec1 = millis();
            if (ok) {
              int outW = (int)(w >> scale);
              int outH = (int)(h >> scale);
              int expBlocks = ((outW + 7) / 8) * ((outH + 7) / 8);
              Serial.printf("AlbumArt: drawn ok, jpg=%ux%u, scale=%d, dst=%dx%d at (%d,%d)\n", w, h, scale, outW, outH, dx, dy);
              Serial.printf("AlbumArt: blocks=%lu/%d clipped=%lu time=%lums\n", (unsigned long)g_tjpg_blk, expBlocks, (unsigned long)g_tjpg_blk_clipped, (unsigned long)(tdec1 - tdec0));
              if ((int)g_tjpg_blk < expBlocks) {
                Serial.println("AlbumArt: WARNING partial decode (progressive?) -> trying JPEGDEC (worker)");
                // Progressive JPEG fallback using JPEGDEC in worker task to avoid loop stack overflow
                JpegdecTaskCtx ctx; ctx.buf = jpg.data(); ctx.len = (int)jpg.size(); ctx.result = 0;
                unsigned long jt0 = millis();
                TaskHandle_t h = nullptr;
                // Use a larger stack (8192 words = 32KB) on core 0
                xTaskCreatePinnedToCore(jpegdec_worker, "jpegdec", 12288, &ctx, tskIDLE_PRIORITY+1, &h, 0);
                // Wait up to 7 seconds for completion
                while (!ctx.done && (millis() - jt0) < 7000) {
                  delay(10);
                }
                unsigned long jt1 = millis();
                if (ctx.result == 1 && g_jpg_fb) {
                  int fbw = g_jpg_fb_w, fbh = g_jpg_fb_h;
                  int dx2 = (480 - fbw) / 2; if (dx2 < 0) dx2 = 0;
                  int dy2 = (480 - fbh) / 2; if (dy2 < 0) dy2 = 0;
                  gfx->fillScreen(BLACK);
                  unsigned long rt0 = millis();
                  for (int row = 0; row < fbh; ++row) {
                    gfx->draw16bitRGBBitmap(dx2, dy2 + row, &g_jpg_fb[row * fbw], fbw, 1);
                  }
                  unsigned long rt1 = millis();
                  Serial.printf("AlbumArt: JPEGDEC drawn ok (worker->fb), src=%dx%d, dst at (%d,%d), time=%lums\n", fbw, fbh, dx2, dy2, (unsigned long)(rt1 - rt0));
                  free(g_jpg_fb); g_jpg_fb = nullptr; g_jpg_fb_w = g_jpg_fb_h = 0;
                } else {
                  Serial.println("AlbumArt: JPEGDEC decode failed (worker)");
                  if (g_jpg_fb) { free(g_jpg_fb); g_jpg_fb = nullptr; g_jpg_fb_w = g_jpg_fb_h = 0; }
                }
              }
            } else {
              Serial.println("AlbumArt: JPEG decode failed");
              Serial.printf("AlbumArt: blocks=%lu clipped=%lu time=%lums\n", (unsigned long)g_tjpg_blk, (unsigned long)g_tjpg_blk_clipped, (unsigned long)(tdec1 - tdec0));
              // Try PNG fallback if it looks like PNG
              bool png_ok = false;
              if (jpg.size() >= 8 && jpg[0]==0x89 && jpg[1]==0x50 && jpg[2]==0x4E && jpg[3]==0x47 && jpg[4]==0x0D && jpg[5]==0x0A && jpg[6]==0x1A && jpg[7]==0x0A) {
                if (g_png.openRAM(jpg.data(), jpg.size(), pngDrawAlbum) == PNG_SUCCESS) {
                  int pw = g_png.getWidth();
                  int ph = g_png.getHeight();
                  int pscale = 1; while (((pw/pscale) > 480 || (ph/pscale) > 480) && pscale < 4) pscale++;
                  g_png_scale = pscale;
                  g_png_out_w = pw / pscale;
                  g_png_out_h = ph / pscale;
                  g_png_center_dx = (480 - g_png_out_w) / 2; if (g_png_center_dx < 0) g_png_center_dx = 0;
                  g_png_center_dy = (480 - g_png_out_h) / 2; if (g_png_center_dy < 0) g_png_center_dy = 0;
                  gfx->fillScreen(BLACK);
                  unsigned long pt0 = millis();
                  int prc = g_png.decode(NULL, 0);
                  unsigned long pt1 = millis();
                  g_png.close();
                  if (prc == PNG_SUCCESS) {
                    Serial.printf("AlbumArt: PNG drawn ok, png=%dx%d, scale=%d, dst=%dx%d at (%d,%d), time=%lums\n",
                                  pw, ph, pscale, g_png_out_w, g_png_out_h, g_png_center_dx, g_png_center_dy, (unsigned long)(pt1-pt0));
                    png_ok = true;
                  } else {
                    Serial.println("AlbumArt: PNG decode failed");
                  }
                } else {
                  Serial.println("AlbumArt: PNG open failed");
                }
              }
              if (!png_ok) {
                draw_center_text("Kein Album Art", WHITE, BLACK);
              }
            }
          } else {
            Serial.println("AlbumArt: not a valid JPEG (size probe failed)");
            // Try PNG fallback if it looks like PNG
            bool png_ok = false;
            if (jpg.size() >= 8 && jpg[0]==0x89 && jpg[1]==0x50 && jpg[2]==0x4E && jpg[3]==0x47 && jpg[4]==0x0D && jpg[5]==0x0A && jpg[6]==0x1A && jpg[7]==0x0A) {
              if (g_png.openRAM(jpg.data(), jpg.size(), pngDrawAlbum) == PNG_SUCCESS) {
                int pw = g_png.getWidth();
                int ph = g_png.getHeight();
                int pscale = 1; while (((pw/pscale) > 480 || (ph/pscale) > 480) && pscale < 4) pscale++;
                g_png_scale = pscale;
                g_png_out_w = pw / pscale;
                g_png_out_h = ph / pscale;
                g_png_center_dx = (480 - g_png_out_w) / 2; if (g_png_center_dx < 0) g_png_center_dx = 0;
                g_png_center_dy = (480 - g_png_out_h) / 2; if (g_png_center_dy < 0) g_png_center_dy = 0;
                gfx->fillScreen(BLACK);
                unsigned long pt0 = millis();
                int prc = g_png.decode(NULL, 0);
                unsigned long pt1 = millis();
                g_png.close();
                if (prc == PNG_SUCCESS) {
                  Serial.printf("AlbumArt: PNG drawn ok, png=%dx%d, scale=%d, dst=%dx%d at (%d,%d), time=%lums\n",
                                pw, ph, pscale, g_png_out_w, g_png_out_h, g_png_center_dx, g_png_center_dy, (unsigned long)(pt1-pt0));
                  png_ok = true;
                } else {
                  Serial.println("AlbumArt: PNG decode failed");
                }
              } else {
                Serial.println("AlbumArt: PNG open failed");
              }
            }
            if (!png_ok) {
              draw_center_text("Kein Album Art", WHITE, BLACK);
            }
          }
        } else {
          Serial.println("AlbumArt: empty body");
          draw_center_text("Kein Album Art", WHITE, BLACK);
        }
        http.end();
      } else {
        Serial.printf("AlbumArt: HTTP error %d\n", code);
        http.end();
        draw_center_text("Kein Album Art", WHITE, BLACK);
#endif
      }
    } else {
      Serial.println("AlbumArt: no URL");
      draw_center_text("Kein Album Art", WHITE, BLACK);
    }
  }

  // Exit on short tap -> back to Config
  if (g_btn_short_released) {
    g_btn_short_released = false;
    g_screen = SCREEN_CONFIG;
    g_config_ui_inited = false;
    return;
  }

}
#endif

// Clean minimal AlbumArt implementation
static void albumart_loop()
{
  if (!g_album_ui_inited) {
    g_album_ui_inited = true;
    gfx->fillScreen(BLACK);
    draw_center_text("Lade Album Art...", WHITE, BLACK);

    // Start non-blocking HTTP download (chunked per loop)
    g_album_file_ready = false;
    g_aa_saved = 0;
    g_aa_last_rx_ms = millis();
    g_aa_last_log_ms = g_aa_last_rx_ms;
    g_aa_cli.setInsecure();
    {
      const String url = "https://i1.sndcdn.com/avatars-NDgaMy6ZFw82pb7S-4Fd2oQ-t500x500.jpg";
      if (g_aa_http.begin(g_aa_cli, url)) {
        g_aa_http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        g_aa_http.useHTTP10(true);
        g_aa_http.setTimeout(15000);
        int code = g_aa_http.GET();
        if (code == HTTP_CODE_OK) {
          g_aa_file = SPIFFS.open("/album.bin", FILE_WRITE);
          if (g_aa_file) {
            g_aa_state = 2;
            Serial.println("AlbumArt: download started");
          } else {
            Serial.println("AlbumArt: file open failed");
            g_aa_http.end();
          }
        } else {
          Serial.printf("AlbumArt: HTTP error %d\n", code);
          g_aa_http.end();
        }
      } else {
        Serial.println("AlbumArt: begin() failed");
      }
    }
  }
  // Draw when background download finished; decode now on main thread (non-blocking until ready)
  if (g_album_file_ready) {
    g_album_file_ready = false;
    const char* path = "/album.bin";
    std::vector<uint8_t> jpg;
    File fi = SPIFFS.open(path, FILE_READ);
    if (fi) {
      const size_t CH = 2048; uint8_t b[CH];
      while (fi.available()) { int n = fi.read(b, CH); if (n <= 0) break; jpg.insert(jpg.end(), b, b+n); }
      fi.close();
    }
    if (!jpg.empty()) {
      TJpgDec.setSwapBytes(false);
      TJpgDec.setCallback(tjpg_output);
      gfx->fillScreen(BLACK);
      TJpgDec.setJpgScale(0);
      TJpgDec.drawJpg(0, 0, jpg.data(), jpg.size());
      Serial.printf("AlbumArt: drawn from file in foreground (%u bytes)\n", (unsigned)jpg.size());
    } else {
      Serial.println("AlbumArt: file empty or read failed");
    }
  }
  // Continue the non-blocking downloader
  if (g_aa_state == 2) {
    WiFiClient *s = g_aa_http.getStreamPtr();
    if (s == nullptr) {
      // finalize early if stream is null
      g_aa_file.close();
      g_aa_http.end();
      g_aa_state = 0;
      g_album_file_ready = (g_aa_saved > 0);
      Serial.println("AlbumArt: stream null, finalize");
      return;
    }
    if (!g_aa_http.connected()) {
      g_aa_file.close();
      g_aa_http.end();
      g_aa_state = 0;
      g_album_file_ready = (g_aa_saved > 0);
      Serial.printf("AlbumArt: disconnected, saved %u bytes\n", (unsigned)g_aa_saved);
      return;
    }
    int avail = s->available();
    if (avail > 0) {
      uint8_t buf[1024];
      int toread = avail; if (toread > (int)sizeof(buf)) toread = sizeof(buf);
      int n = s->readBytes((char*)buf, toread);
      if (n > 0) {
        g_aa_file.write(buf, n);
        g_aa_saved += n;
        g_aa_last_rx_ms = millis();
        unsigned long now = g_aa_last_rx_ms;
        if (now - g_aa_last_log_ms > 500) { g_aa_last_log_ms = now; Serial.printf("AlbumArt: recv %u bytes...\n", (unsigned)g_aa_saved); }
      }
    } else {
      // If no progress for >1500ms, finalize
      unsigned long now = millis();
      if ((now - g_aa_last_rx_ms) > 1500) {
        g_aa_file.close();
        g_aa_http.end();
        g_aa_state = 0;
        g_album_file_ready = true;
        Serial.printf("AlbumArt: saved %u bytes\n", (unsigned)g_aa_saved);
      }
    }
  }

  if (g_btn_short_released) {
    g_btn_short_released = false;
    g_screen = SCREEN_CONFIG;
    g_config_ui_inited = false;
    return;
  }
}



void loop()
{
  // Update global button state and handle long-press -> always return to Player
  {
    uint8_t edge = 0, b = 0; unsigned long tedge = 0;
    noInterrupts();
    if (g_btn_edge) { edge = 1; b = g_btn_raw; tedge = g_btn_edge_time; g_btn_edge = 0; }
    else { b = g_btn_raw; }
    interrupts();

    if (edge) {
      if (b == 1) {
        g_btn_down_since = tedge;
        g_btn_long_handled = false;
      } else {
        unsigned long dur = tedge - g_btn_down_since;
        if (dur < BTN_LONG_MS) g_btn_short_released = true; // short tap event
      }
      g_btn_prev_global = b;
    }
    // Fire long-press once while still holding
    if (b == 1 && !g_btn_long_handled && (millis() - g_btn_down_since) >= BTN_LONG_MS) {
      g_btn_long_handled = true;
      // Ignore long-press on AlbumArt screen (show until short press)
      if (g_screen != SCREEN_PLAYER && g_screen != SCREEN_CONFIG_ALBUM) {
        Serial.println("Button: LONG -> back to Player");
        g_screen = SCREEN_PLAYER;

        g_player_ui_inited = false;
        g_config_ui_inited = false;
        g_brightness_ui_inited = false;
        g_about_ui_inited = false;
        g_room_ui_inited = false;
        g_album_ui_inited = false;
      } else if (g_screen == SCREEN_CONFIG_ALBUM) {
        Serial.println("Button: LONG ignored on AlbumArt");
      }
    }
  }

  // Background room scan (runs regardless of current screen)
  if (g_wifi_ok && millis() - g_room_last_bg_scan >= ROOM_BG_SCAN_INTERVAL_MS) {
    g_room_last_bg_scan = millis();
    Serial.println("Rooms: background scan...");
    scan_sonos_rooms(600, true); // short SSDP window to avoid UI stutter
    if (g_screen == SCREEN_CONFIG_ROOM && g_room_ui_inited) {
      draw_room_list();
    }
  }

  // If WiFi OK, run Player or Config UI
  if (g_wifi_ok)
  {
    if (g_screen == SCREEN_PLAYER) {
      if (!g_player_ui_inited) player_init();
      player_loop();
    } else if (g_screen == SCREEN_CONFIG) {
      config_loop();
    } else if (g_screen == SCREEN_CONFIG_BRIGHTNESS) {
      brightness_loop();
    } else if (g_screen == SCREEN_CONFIG_ROOM) {
      room_loop();
    } else if (g_screen == SCREEN_CONFIG_ABOUT) {
      about_loop();
    } else if (g_screen == SCREEN_CONFIG_ALBUM) {
      albumart_loop();
    }
    delay(20);
    return;
  }

  int tx, ty;
  if (read_touch(&tx, &ty) == 1)
  {
    x_touch = tx; y_touch = ty;
    draw_page();
    Serial.printf("Touch %d\t%d\n", x_touch, y_touch);
  }

  int btn = digitalRead(BUTTON_PIN) == LOW ? 1 : 0;
  if (btn != button_flag)
  {


    button_flag = btn;
    Serial.printf("Button %s\n", button_flag ? "Press" : "Release");
    draw_page();
  }

  if (move_flag)
  {
    move_flag = 0;
    Serial.printf("Encoder: %d\n", encoder_counter);
    draw_page();
  }

  delay(50);
}

