#include "sonos.h"

static String httpGetText(const String &url) {
  HTTPClient http;
  if (!http.begin(url)) return String();
  http.setTimeout(1200);
  http.addHeader("Connection", "close");
  int code = http.GET();
  String out;
  if (code == HTTP_CODE_OK) out = http.getString();
  http.end();
  return out;
}

bool SonosClient::_parseRoomFromDeviceDesc(const String &xml, String &room) {
  // Sonos device description typically contains <roomName>...</roomName>
  int a = xml.indexOf("<roomName>");
  if (a < 0) return false;
  a += 10; // len("<roomName>")
  int b = xml.indexOf("</roomName>", a);
  if (b < 0) return false;
  room = xml.substring(a, b);
  room.trim();
  return room.length() > 0;
}


bool SonosClient::connectKnown(const String &baseURL, const String &roomName) {
  _baseURL = baseURL;
  _roomName = roomName;
  _ready = (_baseURL.length() > 0 && _roomName.length() > 0);
  if (_ready) {
    Serial.printf("Rooms: connectKnown room=\"%s\" base=%s\n", _roomName.c_str(), _baseURL.c_str());
    // Ensure we talk to the group's coordinator to avoid 500 errors on non-coordinator members
    bool sw = _switchToCoordinator();
    if (sw) {
      Serial.println("Sonos: coordinator selected after connect");
    }
  }
  return _ready;
}

bool SonosClient::discoverRoom(const String &roomName, uint32_t timeout_ms) {
  _ready = false; _baseURL = ""; _roomName = "";

  WiFiUDP udp;
  // Bind ephemeral port so unicast replies from devices are received reliably
  udp.begin(0);

  // Send two M-SEARCH variants (mirrors UI scanner for robustness)
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

  uint32_t start = millis();
  char buf[1024];
  while (millis() - start < timeout_ms) {
    int pkt = udp.parsePacket();
    if (pkt > 0) {
      int n = udp.read((uint8_t*)buf, sizeof(buf)-1);
      if (n <= 0) continue;
      buf[n] = 0;
      String resp(buf);
      // Normalize CRLF to LF for header search
      resp.replace("\r", "\n");
      // Find LOCATION header
      int lh = resp.indexOf("\nLOCATION:");
      if (lh < 0) lh = resp.indexOf("\nLocation:");
      if (lh < 0) lh = resp.indexOf("\nlocation:");
      if (lh >= 0) {
        int eol = resp.indexOf('\n', lh+1);
        String line = resp.substring(lh, eol >= 0 ? eol : resp.length());
        int p = line.indexOf(' ');
        if (p >= 0) {
          String url = line.substring(p+1); url.trim();
          // Fetch device description
          String desc = httpGetText(url);
          String r;
          if (_parseRoomFromDeviceDesc(desc, r)) {
            if (r.equalsIgnoreCase(roomName)) {
              // derive base URL from LOCATION (scheme://host:port)
              int schemeEnd = url.indexOf("://");
              int hostStart = (schemeEnd > 0) ? schemeEnd + 3 : 0;
              int pathStart = url.indexOf('/', hostStart);
              String base = (pathStart > 0) ? url.substring(0, pathStart) : url;
              _baseURL = base; _roomName = r; _ready = true;
              udp.stop();
              return true;
            }
          }
        }
      }
    } else {
      delay(15);
    }
  }
  udp.stop();
  return false;
}

String SonosClient::_extractTag(const String &xml, const char *tagBegin, const char *tagEnd) {
  int a = xml.indexOf(tagBegin);
  if (a < 0) return String();
  a += strlen(tagBegin);
  int b = xml.indexOf(tagEnd, a);
  if (b < 0) return String();
  String v = xml.substring(a, b);
  v.trim();
  return v;
}

String SonosClient::_unescapeEntities(String s) {
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("&#39;", "'");
  s.replace("&amp;", "&");
  return s;
}

bool SonosClient::_switchToCoordinator() {
  // Query ZoneGroupTopology for current groups and coordinators
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:GetZoneGroupState xmlns:u=\"urn:schemas-upnp-org:service:ZoneGroupTopology:1\"/>"
    "</s:Body></s:Envelope>";
  String resp;
  if (!_soapPOST("/ZoneGroupTopology/Control", "\"urn:schemas-upnp-org:service:ZoneGroupTopology:1#GetZoneGroupState\"", body, resp)) {
    return false;
  }
  String zgs = _extractTag(resp, "<ZoneGroupState>", "</ZoneGroupState>");
  if (!zgs.length()) return false;
  String xml = _unescapeEntities(zgs);
  // Find the ZoneGroup containing our room name
  String needle = String("ZoneName=\"") + _roomName + "\"";
  int namePos = xml.indexOf(needle);
  if (namePos < 0) return false;
  int groupStart = xml.lastIndexOf("<ZoneGroup ", namePos);
  if (groupStart < 0) return false;
  // Extract Coordinator attribute from ZoneGroup
  int coordPos = xml.indexOf("Coordinator=\"", groupStart);
  if (coordPos < 0) return false;
  coordPos += 13; // len("Coordinator=\"")
  int coordEnd = xml.indexOf('"', coordPos);
  if (coordEnd < 0) return false;
  String coordUUID = xml.substring(coordPos, coordEnd);
  // Find the ZoneGroupMember with that UUID and extract Location
  String uuidNeedle = String("<ZoneGroupMember UUID=\"") + coordUUID + "\"";
  int memPos = xml.indexOf(uuidNeedle, groupStart);
  if (memPos < 0) memPos = xml.indexOf(uuidNeedle); // fallback global
  if (memPos < 0) return false;
  int locPos = xml.indexOf("Location=\"", memPos);
  if (locPos < 0) return false;
  locPos += 10; // len("Location=\"")
  int locEnd = xml.indexOf('"', locPos);
  if (locEnd < 0) return false;
  String location = xml.substring(locPos, locEnd);
  // Derive base URL from location (scheme://host:port/...)
  int schemeEnd = location.indexOf("://");
  int hostStart = (schemeEnd > 0) ? schemeEnd + 3 : 0;
  int pathStart = location.indexOf('/', hostStart);
  String newBase = (pathStart > 0) ? location.substring(0, pathStart) : location;
  if (newBase.length() && newBase != _baseURL) {
    Serial.printf("Sonos: switched to coordinator base=%s\n", newBase.c_str());
    _baseURL = newBase;
    return true;
  }
  return false;
}

bool SonosClient::_soapPOST(const String &controlPath, const String &soapAction, const String &body, String &resp) {
  String url = _baseURL + controlPath; // e.g. /MediaRenderer/RenderingControl/Control
  HTTPClient http;
  if (!http.begin(url)) { Serial.println("Sonos DBG: http.begin failed"); _lastHTTP = -1; return false; }
  http.setTimeout(1200);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", soapAction);
  http.addHeader("Connection", "close");
  int code = http.POST(body);
  _lastHTTP = code;
  String bodyText;
  if (code > 0) {
    bodyText = http.getString();
  }
  if (code == HTTP_CODE_OK) {
    resp = bodyText;
    http.end();
    return true;
  }
  // Log SOAP fault snippet if present
  if (bodyText.length()) {
    String snip = bodyText.substring(0, min(300, (int)bodyText.length()));
    snip.replace('\r', ' ');
    Serial.printf("Sonos DBG: SOAP POST fail path=%s action=%s http=%d body: %s\n", controlPath.c_str(), soapAction.c_str(), code, snip.c_str());
  } else {
    Serial.printf("Sonos DBG: SOAP POST fail path=%s action=%s http=%d\n", controlPath.c_str(), soapAction.c_str(), code);
  }
  http.end();
  return false;
}

bool SonosClient::poll(SonosState &out) {
  if (!_ready) return false;
  bool changed = false;

  // 1) Volume
  {
    String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:GetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
      "<InstanceID>0</InstanceID><Channel>Master</Channel>"
      "</u:GetVolume>"
      "</s:Body></s:Envelope>";
    String resp;
    if (_soapPOST("/MediaRenderer/RenderingControl/Control", "\"urn:schemas-upnp-org:service:RenderingControl:1#GetVolume\"", body, resp)) {
      String v = _extractTag(resp, "<CurrentVolume>", "</CurrentVolume>");
      if (v.length() > 0) {
        int iv = constrain(v.toInt(), 0, 100);
        if (out.volume != iv) { out.volume = iv; changed = true; }
      }
    }
  }

  // 2) Transport state (PLAYING/PAUSED_PLAYBACK/STOPPED/TRANSITIONING)
  {
    String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID>"
      "</u:GetTransportInfo>"
      "</s:Body></s:Envelope>";
    String resp;
    bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo\"", body, resp);
    if (!ok && _lastHTTP == 500) {
      if (_switchToCoordinator()) {
        ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo\"", body, resp);
      }
    }
    if (ok) {
      String st = _extractTag(resp, "<CurrentTransportState>", "</CurrentTransportState>");
      if (st.length() > 0) {
        Serial.printf("Sonos DBG: transport parsed len=%d val=[%s]\n", st.length(), st.c_str());
        if (out.transportState != st) { out.transportState = st; changed = true; }
        bool pl = (st == "PLAYING" || st == "TRANSITIONING");
        if (out.playing != pl) { out.playing = pl; changed = true; }
      } else {
        // Debug: show SOAP response if parsing failed
        Serial.println("Sonos DBG: GetTransportInfo response (truncated):");
        String dbg = resp.substring(0, min((int)resp.length(), 300));
        dbg.replace('\r', ' ');
        Serial.println(dbg);
      }
    }
  }

  // 3) PositionInfo: RelTime, Duration; and metadata (title, artist, album, albumArt)
  {
    String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID><Channel>Master</Channel>"
      "</u:GetPositionInfo>"
      "</s:Body></s:Envelope>";
    String resp;
    bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\"", body, resp);
    if (!ok && _lastHTTP == 500) {
      if (_switchToCoordinator()) {
        ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\"", body, resp);
      }
    }
    if (ok) {
      // Top-level times
      String rt = _extractTag(resp, "<RelTime>", "</RelTime>");
      String du = _extractTag(resp, "<TrackDuration>", "</TrackDuration>");
      if (rt.length() && out.relTime != rt) { out.relTime = rt; changed = true; }
      if (du.length() && out.duration != du) { out.duration = du; changed = true; }

      // Extract TrackMetaData block
      String md = _extractTag(resp, "<TrackMetaData>", "</TrackMetaData>");
      if (md.length() == 0) md = _extractTag(resp, "<TrackMetaData xsi:type=\"string\">", "</TrackMetaData>");
      if (md.length() > 0) {
        // Unescape minimal XML entities often used
        md.replace("&lt;", "<");
        md.replace("&gt;", ">");
        md.replace("&amp;", "&");
        String title  = _extractTag(md, "<dc:title>", "</dc:title>");
        String artist = _extractTag(md, "<upnp:artist>", "</upnp:artist>");
        if (artist.length() == 0) artist = _extractTag(md, "<dc:creator>", "</dc:creator>");
        String album  = _extractTag(md, "<upnp:album>", "</upnp:album>");
        String art    = _extractTag(md, "<upnp:albumArtURI>", "</upnp:albumArtURI>");
        // Unescape any remaining entities inside fields
        title = _unescapeEntities(title);
        artist = _unescapeEntities(artist);
        album = _unescapeEntities(album);
        art = _unescapeEntities(art);
        if (title.length()  && out.title  != title)  { out.title  = title;  changed = true; }
        if (artist.length() && out.artist != artist) { out.artist = artist; changed = true; }
        if (album.length()  && out.album  != album)  { out.album  = album;  changed = true; }
        if (art.length() > 0) {
          if (art.startsWith("/")) art = _baseURL + art; // normalize
          if (out.albumArtURI != art) { out.albumArtURI = art; changed = true; }
        }
      }
    }
  }

  return changed;
}



bool SonosClient::seekRelTime(const String &hhmmss) {
  if (!_ready) return false;
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:Seek xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit>"
    "<Target>" + hhmmss + "</Target>"
    "</u:Seek>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Seek\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Seek\"", body, resp);
    }
  }
  if (ok) {
    Serial.printf("Sonos: seek to %s\n", hhmmss.c_str());
  }
  return ok;
}

bool SonosClient::setVolume(int pct) {
  if (!_ready) return false;
  pct = constrain(pct, 0, 100);
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:SetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
    "<InstanceID>0</InstanceID><Channel>Master</Channel>"
    "<DesiredVolume>" + String(pct) + "</DesiredVolume>"
    "</u:SetVolume>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/RenderingControl/Control", "\"urn:schemas-upnp-org:service:RenderingControl:1#SetVolume\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/RenderingControl/Control", "\"urn:schemas-upnp-org:service:RenderingControl:1#SetVolume\"", body, resp);
    }
  }
  if (ok) Serial.printf("Sonos: set volume %d\n", pct);
  return ok;
}

bool SonosClient::play() {
  if (!_ready) return false;
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID><Speed>1</Speed>"
    "</u:Play>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"", body, resp);
    }
  }
  if (ok) Serial.println("Sonos: play()");
  return ok;
}

bool SonosClient::pause() {
  if (!_ready) return false;
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:Pause>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Pause\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Pause\"", body, resp);
    }
  }
  if (ok) Serial.println("Sonos: pause()");
  return ok;
}

bool SonosClient::next() {
  if (!_ready) return false;
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:Next xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:Next>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Next\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Next\"", body, resp);
    }
  }
  if (ok) Serial.println("Sonos: next()");
  return ok;
}

bool SonosClient::previous() {
  if (!_ready) return false;
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:Previous xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:Previous>"
    "</s:Body></s:Envelope>";
  String resp;
  bool ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Previous\"", body, resp);
  if (!ok && _lastHTTP == 500) {
    if (_switchToCoordinator()) {
      ok = _soapPOST("/MediaRenderer/AVTransport/Control", "\"urn:schemas-upnp-org:service:AVTransport:1#Previous\"", body, resp);
    }
  }
  if (ok) Serial.println("Sonos: previous()");
  return ok;
}


