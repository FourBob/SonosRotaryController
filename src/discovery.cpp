#include "discovery.h"

DiscoveryManager& DiscoveryManager::instance() {
  static DiscoveryManager inst;
  return inst;
}

void DiscoveryManager::pause() { _paused = true; }
void DiscoveryManager::resume() { _paused = false; }
bool DiscoveryManager::isPaused() const { return _paused; }

static bool parseLocationFromSSDP(const String& resp, String& location) {
  String up = resp;
  up.replace("\r", "\n");
  int lh = up.indexOf("\nLOCATION:"); if (lh < 0) lh = up.indexOf("\nLocation:");
  if (lh < 0) return false;
  int le = up.indexOf('\n', lh+1); if (le < 0) le = up.length();
  String line = up.substring(lh+10, le);
  line.trim();
  if (!line.startsWith("http")) return false;
  location = line;
  return true;
}

static bool parseRoomFromDeviceDesc(const String& xml, String& room) {
  int a = xml.indexOf("<roomName>"); if (a < 0) return false; a += 10;
  int b = xml.indexOf("</roomName>", a); if (b < 0) return false;
  room = xml.substring(a, b); room.trim();
  return room.length() > 0;
}

static String baseFromLocation(const String& url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd > 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  return (pathStart > 0) ? url.substring(0, pathStart) : url;
}

void DiscoveryManager::mergeBurst(uint32_t window_ms) {
  if (_paused) { LOGD("Discovery", "Rooms: mergeBurst skipped (paused)"); return; }

  WiFiUDP udp; udp.begin(0);
  const char *msearch1 =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
    "USER-AGENT: ESP32/3.3.0 UPnP/1.1 PIO/1.0\r\n\r\n";
  const char *msearch2 =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "USER-AGENT: ESP32/3.3.0 UPnP/1.1 PIO/1.0\r\n\r\n";
  udp.beginPacket(IPAddress(239,255,255,250), 1900); udp.write((const uint8_t*)msearch1, strlen(msearch1)); udp.endPacket();
  udp.beginPacket(IPAddress(239,255,255,250), 1900); udp.write((const uint8_t*)msearch2, strlen(msearch2)); udp.endPacket();

  // 1) SSDP-Fenster: Nur Sonos-LOCATIONs sammeln, keine HTTPs im Fenster
  std::vector<String> sonosLocs;

  uint32_t start = millis();
  char buf[1024];
  while (millis() - start < window_ms) {
    int pkt = udp.parsePacket();
    if (pkt <= 0) { delay(10); continue; }
    int n = udp.read((uint8_t*)buf, sizeof(buf)-1); if (n <= 0) continue; buf[n] = 0;
    String resp(buf);

    // Nur Sonos-Antworten akzeptieren (Server/ST/USN enthalten Sonos/ZonePlayer/RINCON)
    String hdr = resp; hdr.toLowerCase();
    bool isSonos = (hdr.indexOf("zoneplayer") >= 0) || (hdr.indexOf("sonos") >= 0) || (hdr.indexOf("rincon") >= 0);
    if (!isSonos) continue;

    String loc; if (!parseLocationFromSSDP(resp, loc)) continue;
    bool dup = false; for (auto &x : sonosLocs) { if (x.equalsIgnoreCase(loc)) { dup = true; break; } }
    if (!dup) sonosLocs.push_back(loc);
  }
  udp.stop();

  // 2) Nach dem Fenster: erst jetzt HTTP-Requests durchführen (blockierend, aber wenige)
  for (const auto& loc : sonosLocs) {
    HTTPClient http;
    if (!http.begin(loc)) continue;
    http.setTimeout(800);
    http.addHeader("Connection", "close");
    int code = http.GET();
    String desc = (code == HTTP_CODE_OK) ? http.getString() : String();
    http.end();
    if (!desc.length()) continue;
    String room; if (!parseRoomFromDeviceDesc(desc, room)) continue;
    String base = baseFromLocation(loc);
    upsert(room, base);
  }
}

void DiscoveryManager::getRooms(std::vector<RoomInfo>& out) const {
  out = _rooms;
  std::sort(out.begin(), out.end(), [](const RoomInfo& a, const RoomInfo& b){ return a.name.compareTo(b.name) < 0; });
}

bool DiscoveryManager::getBaseFor(const String& room, String& base) const {
  for (const auto& r : _rooms) {
    if (r.name.equalsIgnoreCase(room)) { base = r.base; return true; }
  }
  return false;
}

void DiscoveryManager::upsert(const String& name, const String& base) {
  uint32_t now = millis();
  for (auto& r : _rooms) {
    if (r.name.equalsIgnoreCase(name)) { r.base = base; r.seenMs = now; return; }
  }
  RoomInfo info; info.name = name; info.base = base; info.seenMs = now;
  _rooms.push_back(info);
  LOGD("Discovery", "SSDP: room=\"%s\" base=%s", name.c_str(), base.c_str());
}

