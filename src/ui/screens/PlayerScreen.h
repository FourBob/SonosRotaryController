#pragma once
#include <memory>
#include <functional>
#include <Arduino.h>
#include "ui/Screen.h"
#include "albumart/BackgroundArt.h"
#include "base/Config.h"
#include "sonos.h"
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>


class Arduino_RGB_Display; extern Arduino_RGB_Display* gfx;

namespace ui {

class PlayerScreen : public IScreen {
public:
  explicit PlayerScreen(albumart::BackgroundArt* bg) : bg_(bg) {}
  const char* name() const override { return "PlayerScreen"; }

  // Public redraw entry points (now implemented here)
  void drawTitleOverlay() { drawTitle_(); }
  void drawPlay()         { drawPlay_(); }
  void drawVolume()       { drawVolume_(); }

  // Touch hit testing
  bool isVolumeIconHit(int tx, int ty) const {
    return (tx >= volHitX_ && tx < volHitX_ + volHitW_ &&
            ty >= volHitY_ && ty < volHitY_ + volHitH_);
  }
  void drawProgress()     { drawProgress_(); }
  void drawAllUi()        { drawPlay(); drawVolume(); drawProgress(); }

  // Data suppliers (avoid globals)
  void setIsPlayingSupplier(std::function<bool()> f){ isPlayingFn_ = std::move(f); }
  void setVolumeSupplier(std::function<int()> f)    { volumeFn_    = std::move(f); }
  void setMutedSupplier(std::function<bool()> f)    { mutedFn_     = std::move(f); }
  void setProgressSupplier(std::function<int()> f)  { progressFn_  = std::move(f); }
  void setTitleSupplier(std::function<String()> f)  { titleFn_     = std::move(f); }
  void setArtistSupplier(std::function<String()> f) { artistFn_    = std::move(f); }
  void setRoomNameSupplier(std::function<String()> f){ roomFn_     = std::move(f); }
  void setRelTimeSupplier(std::function<String()> f){ relTimeFn_   = std::move(f); }
  void setDurationSupplier(std::function<String()> f){ durationFn_ = std::move(f); }

  // Backward-compatible: allow external callback override for title if needed
  void setOverlayDrawer(std::function<void()> fn)   { overlayDrawCb_ = std::move(fn); }

  void enter() override { reset(); }
  void exit() override {}

  // Force redraw of all UI elements (call when returning from other screens)
  void reset() {
    Serial.println("PlayerScreen: reset() called");
    prevVol_ = -1;
    prevMuted_ = false;
    prevRoom_ = "";
    // Reset touch hit areas
    volHitX_ = 0; volHitY_ = 0; volHitW_ = 0; volHitH_ = 0;

    // Clear the entire screen first (config menu may have left artifacts)
    if (gfx) {
      Serial.println("PlayerScreen: clearing screen before forceFullRedraw");
      gfx->fillScreen(RGB(0,0,0));  // Clear to black
    }

    // Force background to redraw completely on next draw() call
    if (bg_) {
      Serial.println("PlayerScreen: bg_ is available");
      if (gfx) {
        Serial.println("PlayerScreen: gfx is available, calling forceFullRedraw");
        ui_gfx::Display disp(gfx);
        bg_->forceFullRedraw(disp);
        Serial.println("PlayerScreen: forceFullRedraw completed");
      } else {
        Serial.println("PlayerScreen: gfx is NULL, cannot call forceFullRedraw");
      }
    } else {
      Serial.println("PlayerScreen: bg_ is NULL");
    }

    // Note: Don't call drawAllUi() here as data suppliers might not be set yet
    // drawAllUi() will be called later in player_init()
  }
  void tick() override { /* later: handle encoder/touch & Sonos updates */ }

  void draw(ui_gfx::Display& d) override {
    if (bg_) {
      bg_->blitStep(d);
      if (bg_->consumeDidBlit()) {
        if (overlayDrawCb_) overlayDrawCb_();
        else drawTitle_();
      }
    }
  }

private:
  // Geometry/colors
  static inline uint16_t RGB(uint8_t r, uint8_t g, uint8_t b){ return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
  static constexpr int BTN_H = 90; static constexpr int BTN_W = 160;
  static constexpr int VOL_ICON_W = 40, VOL_ICON_H = 40, VOL_ICON_Y = 12;
  static constexpr int PRG_X = 40, PRG_W = 400, PRG_H = 12, PRG_Y = 480 - BTN_H - 30;

  // Helpers
  static String asciiFallback_(const String &in) {
    String out; out.reserve(in.length()+8);
    for (int i = 0; i < (int)in.length(); ) {
      uint8_t c = (uint8_t)in[i];
      if (c < 0x80) { out += (char)c; ++i; continue; }
      // Umlauts and common smart quotes (very small subset)
      if ((uint8_t)in[i] == 0xC3 && i+1 < (int)in.length()) {
        uint8_t d = (uint8_t)in[i+1];
        switch (d) { case 0x84: out += 'A'; break; case 0x96: out += 'O'; break; case 0x9C: out += 'U'; break;
                     case 0xA4: out += 'a'; break; case 0xB6: out += 'o'; break; case 0xBC: out += 'u'; break;
                     case 0x9F: out += "ss"; break; default: out += '?'; break; }
        i += 2; continue;
      }
      // quotes/dash (very common)
      if ((uint8_t)in[i] == 0xE2 && i+2 < (int)in.length()) { out += '-'; i += 3; continue; }
      out += '?'; ++i;
    }
    return out;
  }
  static void drawTextOutline_(int16_t x, int16_t y, const String& s) {
    gfx->setTextWrap(false);
    gfx->setTextColor(BLACK);
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) { if (!dx && !dy) continue; gfx->setCursor(x+dx, y+dy); gfx->print(s); }
    gfx->setTextColor(WHITE); gfx->setCursor(x, y); gfx->print(s);
  }
  static void drawIconPlay_(int cx, int cy, uint16_t col){ int s=36; gfx->fillTriangle(cx-s/2,cy-s/2, cx-s/2,cy+s/2, cx+s/2,cy, col); }
  static void drawIconPause_(int cx, int cy, uint16_t col){ int h=40,w=12,g=10; gfx->fillRect(cx-g/2-w, cy-h/2, w,h,col); gfx->fillRect(cx+g/2, cy-h/2, w,h,col);}
  static void drawIconPrev_(int cx, int cy, uint16_t col){ int s=36,g=6,cxL=cx-(s/2+g/2),cxR=cx+(s/2+g/2); gfx->fillTriangle(cxL+s/2,cy-s/2,cxL+s/2,cy+s/2,cxL-s/2,cy,col); gfx->fillTriangle(cxR+s/2,cy-s/2,cxR+s/2,cy+s/2,cxR-s/2,cy,col);}
  static void drawIconNext_(int cx, int cy, uint16_t col){ int s=36,g=6,cxL=cx-(s/2+g/2),cxR=cx+(s/2+g/2); gfx->fillTriangle(cxL-s/2,cy-s/2,cxL-s/2,cy+s/2,cxL+s/2,cy,col); gfx->fillTriangle(cxR-s/2,cy-s/2,cxR-s/2,cy+s/2,cxR+s/2,cy,col);}
  static void drawSpeaker_(int x, int y, bool muted) {
    // For now, use improved primitive drawing (PNG integration can be added later)
    uint16_t FG = WHITE;

    // Draw speaker body (rectangle)
    int bodyW = 12, bodyH = 16;
    int bodyX = x + 8, bodyY = y + 12;
    gfx->fillRect(bodyX, bodyY, bodyW, bodyH, FG);

    // Draw speaker cone (triangle)
    int coneX = bodyX + bodyW;
    int coneY1 = bodyY + 2, coneY2 = bodyY + bodyH - 2;
    int coneW = 8;
    gfx->fillTriangle(coneX, coneY1, coneX, coneY2, coneX + coneW, bodyY + bodyH/2, FG);

    // Draw sound waves (if not muted)
    if (!muted) {
      int waveX = coneX + coneW + 2;
      int centerY = y + VOL_ICON_H/2;
      gfx->drawCircle(waveX, centerY, 6, FG);
      gfx->drawCircle(waveX, centerY, 10, FG);
    } else {
      // Draw mute X (thick lines for better visibility)
      uint16_t red = RGB(255, 0, 0);
      int cx = x + VOL_ICON_W/2, cy = y + VOL_ICON_H/2;
      // Draw thick X with multiple parallel lines
      for (int i = -1; i <= 1; i++) {
        gfx->drawLine(cx-8+i, cy-8, cx+8+i, cy+8, red);
        gfx->drawLine(cx-8+i, cy+8, cx+8+i, cy-8, red);
        gfx->drawLine(cx-8, cy-8+i, cx+8, cy+8+i, red);
        gfx->drawLine(cx-8, cy+8+i, cx+8, cy-8+i, red);
      }
    }
  }

  void drawTitle_() {
    const int maxW = 440; const int y_center = 240; const int gap = 6;
    // Title small font
    gfx->setFont(&FreeSansBold12pt7b); gfx->setTextColor(WHITE);
    // Calculate overlay region for background restoration - MUCH LARGER AREA
    // Cover the entire text overlay region generously to avoid text ghosting
    int bandY = y_center - 80;  // Start well above text area
    int bandH = 160;            // Cover 160 pixels height (enough for 4-6 lines)
    if (bandY < 80) bandY = 80; // Don't overlap with header
    if (bandY + bandH > 390) bandH = 390 - bandY; // Don't overlap with buttons
    // Restore background in overlay region (instead of black band)
    if (bg_) {
      ui_gfx::Display disp(gfx);
      bg_->blitRegion(disp, bandY, bandY + bandH - 1);
    }

    auto wrap_and_draw = [&](String s, int y)->int{
      s.trim(); if (!s.length()) return 0; s = asciiFallback_(s);
      // word wrap into up to two lines within maxW
      std::vector<String> words; int start=0; while (start < (int)s.length()){ int sp = s.indexOf(' ', start); if (sp<0){ words.push_back(s.substring(start)); break;} words.push_back(s.substring(start, sp+1)); start=sp+1; }
      auto measure=[&](const String& line){ int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds(line.c_str(),0,0,&bx,&by,&bw,&bh); return (int)bw; };
      auto draw_line_centered=[&](const String& line, int yy){ int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds(line.c_str(),0,0,&bx,&by,&bw,&bh); int x=(480-(int)bw)/2; drawTextOutline_(x, yy, line); return (int)bh; };
      String l1,l2; int idx=0; while(idx<(int)words.size()){ String t=l1+words[idx]; if (measure(t)<=maxW){ l1=t; idx++; } else break; }
      while(idx<(int)words.size()){ String t=l2+words[idx]; if (measure(t)<=maxW){ l2=t; idx++; } else break; }
      int h1=0,h2=0; if (l1.length()) h1=draw_line_centered(l1,y); if (l2.length()) h2=draw_line_centered(l2, y + h1 + gap); return h1 + (l2.length()? (gap+h2):0);
    };
    String t1 = titleFn_ ? titleFn_() : String();
    String t2 = artistFn_ ? artistFn_() : String();
    int title_y = y_center - 24; int title_h = wrap_and_draw(t1, title_y);
    int artist_y = title_y + title_h + 10; wrap_and_draw(t2, artist_y);
  }

  void drawPlay_() {
    int y0 = 480 - BTN_H; gfx->fillRect(BTN_W, y0, BTN_W, BTN_H, RGB(0,0,0));
    int cx = BTN_W + BTN_W/2; int cy = y0 + BTN_H/2; if (isPlayingFn_ ? isPlayingFn_() : false) drawIconPause_(cx, cy, WHITE); else drawIconPlay_(cx, cy, WHITE);
  }

  void drawVolume_() {
    int vol = volumeFn_ ? volumeFn_() : 0; bool muted = mutedFn_ ? mutedFn_() : false; int effective = muted ? 0 : vol; String room = asciiFallback_(roomFn_ ? roomFn_() : String());
    if (prevVol_ == effective && prevMuted_ == muted && prevRoom_ == room) return;
    prevVol_ = effective; prevMuted_ = muted; prevRoom_ = room;
    const uint16_t header_bg = RGB(10,10,10); int band_y0 = 0; int band_h = 80;
    // DON'T fill the entire header area - only draw backgrounds for specific elements
    // gfx->fillRect(0, band_y0, 480, band_h, header_bg);  // REMOVED: This overwrites album art!
    char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%d%%", effective);
    gfx->setFont(&FreeSansBold18pt7b); int16_t tbx, tby; uint16_t tbw, tbh; gfx->getTextBounds(pbuf, 0, 0, &tbx, &tby, &tbw, &tbh);
    const int padding = 10; int total_w = VOL_ICON_W + padding + (int)tbw; int start_x = (480 - total_w) / 2;

    // Update touch hit area (cover icon AND percentage text for easier mute toggle)
    int hit_left = start_x - 40; if (hit_left < 0) hit_left = 0;
    int hit_right = start_x + total_w + 20; if (hit_right > 480) hit_right = 480;
    volHitX_ = hit_left; volHitY_ = 0; volHitW_ = hit_right - hit_left; volHitH_ = band_h;

    // Draw background only for the volume icon and text area
    int icon_bg_x = start_x - 5; int icon_bg_w = total_w + 10; int icon_bg_y = VOL_ICON_Y - 5; int icon_bg_h = VOL_ICON_H + 10;
    gfx->fillRect(icon_bg_x, icon_bg_y, icon_bg_w, icon_bg_h, header_bg);

    gfx->setTextColor(WHITE, header_bg);
    int icon_x = start_x; drawSpeaker_(icon_x, VOL_ICON_Y, muted);
    int text_x = start_x + VOL_ICON_W + padding; int text_base_y = VOL_ICON_Y + (VOL_ICON_H + (int)tbh) / 2 - 2; gfx->setCursor(text_x, text_base_y); gfx->print(pbuf);

    // Draw background only for the room name area
    if (room.length()) {
      gfx->setFont(&FreeSansBold12pt7b); int16_t rbx, rby; uint16_t rbw, rbh; gfx->getTextBounds(room.c_str(),0,0,&rbx,&rby,&rbw,&rbh);
      int rx = 240 - (int)rbw/2; int ry = band_y0 + band_h - 10;
      // Draw background only for room text
      gfx->fillRect(rx - 5, ry - rbh - 2, rbw + 10, rbh + 4, header_bg);
      gfx->setTextColor(WHITE, header_bg); gfx->setCursor(rx, ry); gfx->print(room);
    }
  }

  void drawProgress_() {
    int pct = progressFn_ ? progressFn_() : 0; gfx->fillRect(0, PRG_Y - 10, 480, PRG_H + 20, RGB(10,10,10));
    gfx->fillRect(PRG_X, PRG_Y, PRG_W, PRG_H, RGB(30,30,30)); int fw = (PRG_W * pct) / 100; gfx->fillRect(PRG_X, PRG_Y, fw, PRG_H, RGB(0,120,255));
    int kx = PRG_X + fw; if (kx < PRG_X) kx = PRG_X; if (kx > PRG_X + PRG_W - 1) kx = PRG_X + PRG_W - 1; int ky = PRG_Y + PRG_H/2; int kr = 9; gfx->fillCircle(kx, ky, kr, WHITE);
    gfx->drawRoundRect(PRG_X-1, PRG_Y-1, PRG_W+2, PRG_H+2, 4, RGB(60,60,60));
    auto fmt_label=[&](const String &s)->String{ int a=s.indexOf(':'); if(a<0) return String(); int b=s.indexOf(':',a+1); if(b<0) return String(); int h=s.substring(0,a).toInt(); int m=s.substring(a+1,b).toInt(); int sec=s.substring(b+1).toInt(); char buf[12]; if(h>0) snprintf(buf,sizeof(buf),"%d:%02d:%02d",h,m,sec); else snprintf(buf,sizeof(buf),"%d:%02d",m,sec); return String(buf); };
    String l = fmt_label(relTimeFn_ ? relTimeFn_() : String()); String r = fmt_label(durationFn_ ? durationFn_() : String()); String du_up=r; du_up.trim(); du_up.toUpperCase(); if (!r.length() || du_up=="NOT_IMPLEMENTED" || du_up=="00:00:00") { r=""; }
    gfx->setFont(&FreeSansBold12pt7b); gfx->setTextColor(WHITE, RGB(10,10,10)); int yText = PRG_Y - 16; if (l.length()) { gfx->setCursor(PRG_X, yText); gfx->print(l); }
    if (r.length()) { int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds(r.c_str(),0,0,&bx,&by,&bw,&bh); int rx = PRG_X + PRG_W - bw; if (rx < PRG_X) rx = PRG_X; gfx->setCursor(rx, yText); gfx->print(r); }
  }

  albumart::BackgroundArt* bg_ = nullptr; // not owned
  std::function<void()> overlayDrawCb_;
  // Data suppliers
  std::function<bool()> isPlayingFn_; std::function<int()> volumeFn_; std::function<bool()> mutedFn_;
  std::function<int()> progressFn_; std::function<String()> titleFn_, artistFn_, roomFn_, relTimeFn_, durationFn_;
  int prevVol_ = -1; bool prevMuted_ = false; String prevRoom_;

  // Touch hit areas (updated by drawVolume_)
  mutable int volHitX_ = 0, volHitY_ = 0, volHitW_ = 0, volHitH_ = 0;
};

} // namespace ui

