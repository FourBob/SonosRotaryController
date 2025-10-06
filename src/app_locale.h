#pragma once

// Simple compile-time locale selection
// Set -DLOCALE_LANG_EN in build_flags to switch to English in the future

#if defined(LOCALE_LANG_EN)
  #define L_ABOUT               "About"
  #define L_BRIGHTNESS          "Brightness"
  #define L_SONOS_ROOM          "Sonos Room"
  #define L_ALBUM_ART           "Album Art"
  #define L_ALBUM_ART_LOG       "Album Art Log"
  #define L_TITLE_INFO          "Title Information"
  #define L_SYSTEM_INFO         "System Information"
  #define L_PLAY                "Play"
  #define L_PAUSE               "Pause"
  #define L_NEXT                "Next"
  #define L_PREV                "Prev"
  #define L_VOLUME              "Volume"
  #define L_NO_TRACK            "No track"
#else
  // Default: German
  #define L_ABOUT               "\u00dcber"
  #define L_BRIGHTNESS          "Helligkeit"
  #define L_SONOS_ROOM          "Sonos Raum"
  #define L_ALBUM_ART           "Album Art"
  #define L_ALBUM_ART_LOG       "Album-Art Log"
  #define L_TITLE_INFO          "Titel-Information"
  #define L_SYSTEM_INFO         "System-Information"
  #define L_PLAY                "Play"
  #define L_PAUSE               "Pause"
  #define L_NEXT                "Weiter"
  #define L_PREV                "Zur\u00fcck"
  #define L_VOLUME              "Lautst\u00e4rke"
  #define L_NO_TRACK            "Kein Titel"
#endif

