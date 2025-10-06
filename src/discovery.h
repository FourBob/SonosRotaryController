#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <vector>
#include "base/Log.h"

struct RoomInfo {
  String name;     // display name
  String base;     // http://ip:1400
  uint32_t seenMs; // millis() when last seen
};

class DiscoveryManager {
public:
  static DiscoveryManager& instance();

  void pause();
  void resume();
  bool isPaused() const;

  // Performs a short SSDP M-SEARCH burst and merges results into cache.
  // Blocking only for window_ms (keep small, e.g., 200-600ms).
  void mergeBurst(uint32_t window_ms);

  // Copies current cache to out (sorted by name ascending).
  void getRooms(std::vector<RoomInfo>& out) const;

  // Returns true and writes base if known for given room (case-insensitive).
  bool getBaseFor(const String& room, String& base) const;

private:
  DiscoveryManager() {}
  void upsert(const String& name, const String& base);

  bool _paused = false;
  std::vector<RoomInfo> _rooms;
};

