#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <FS.h>
#include <SPIFFS.h>
#include <memory>
#include "base/Config.h"
#include "base/Log.h"

namespace albumart {

class Downloader {
public:
  // Returns true on success; saves to SPIFFS path
  static bool downloadToFile(const String& url, const char* path) {
    using namespace sys;
    HTTPClient http;
    bool begun = false;
    std::unique_ptr<WiFiClientSecure> httpsCli;
    if (url.startsWith("https://")) {
      httpsCli.reset(new WiFiClientSecure());
      httpsCli->setInsecure();
      begun = http.begin(*httpsCli, url);
    } else {
      begun = http.begin(url);
    }
    if (!begun) { LOGE("DL","begin failed"); return false; }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    // Align with foreground downloader: force HTTP/1.0 for reliable Content-Length
    http.useHTTP10(true);
    // Be explicit about closing and a UA some CDNs like
    http.addHeader("Connection", "close");
    http.setUserAgent("ESP32-Sonos/1.0");
    http.setTimeout(kHttpTimeoutMs);
    int code = http.GET();
    if (code != HTTP_CODE_OK) { LOGE("DL","HTTP %d %s", code, http.errorToString(code).c_str()); http.end(); return false; }
    int total = http.getSize();
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) { LOGE("DL","SPIFFS open %s failed", path); http.end(); return false; }

    // Read like the foreground path (robust on i.scdn.co)
    const size_t CH = 2048;
    uint8_t buf[CH];
    WiFiClient* stream = http.getStreamPtr();
    size_t saved = 0;
    unsigned long last_rx = millis();
    while (true) {
      int avail = stream ? stream->available() : 0;
      if (avail > 0) {
        size_t to_read = (avail > (int)CH) ? CH : (size_t)avail;
        int n = stream->readBytes((char*)buf, to_read);
        if (n > 0) { f.write(buf, n); saved += (size_t)n; last_rx = millis(); }
      } else {
        // No data pending, check end conditions
        if (total >= 0 && (int)saved >= total) break; // read expected bytes
        if ((!http.connected()) && (!avail)) break;   // connection closed
        if (millis() - last_rx > kHttpInactivityMs) break; // inactivity timeout
        delay(10);
      }
    }
    f.close();
    LOGI("DL","saved %u/%d to %s", (unsigned)saved, total, path);
    http.end();
    return saved > 0 && (total < 0 || (int)saved == total);
  }
};

} // namespace albumart

