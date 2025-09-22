#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

struct SonosState {
  bool   playing = false;
  int    volume = -1;       // 0..100
  // Transport
  String transportState;    // e.g. PLAYING, PAUSED_PLAYBACK, STOPPED, TRANSITIONING
  // Track metadata
  String title;
  String artist;
  String album;
  String relTime;           // e.g. 00:01:23
  String duration;          // e.g. 00:03:45
  String albumArtURI;       // absolute URL after normalization
};

class SonosClient {
public:
  bool discoverRoom(const String &roomName, uint32_t timeout_ms = 2000);
  // Directly set known base URL and room name (from prior scan)
  bool connectKnown(const String &baseURL, const String &roomName);
  bool isReady() const { return _ready; }
  // Polls rendering/transport state. Returns true if new data parsed.
  bool poll(SonosState &out);
  // Control APIs
  bool seekRelTime(const String &hhmmss);
  bool setVolume(int pct);
  bool play();
  bool pause();
  bool next();
  bool previous();

  String baseURL() const { return _baseURL; }
  String roomName() const { return _roomName; }

private:
  bool _ready = false;
  String _baseURL; // e.g. http://192.168.1.50:1400
  String _roomName;
  int    _lastHTTP = 0; // last HTTP code from SOAP

  bool _parseRoomFromDeviceDesc(const String &xml, String &room);
  bool _soapPOST(const String &controlPath, const String &soapAction, const String &body, String &resp);
  static String _extractTag(const String &xml, const char *tagBegin, const char *tagEnd);
  static String _unescapeEntities(String s);
  bool _switchToCoordinator();
};

