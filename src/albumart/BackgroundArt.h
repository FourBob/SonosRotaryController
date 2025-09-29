#pragma once
#include <Arduino.h>
#include "gfx/Display.h"

namespace albumart {

// BackgroundArt kapselt Download, Decode und Blit des Album-Art-Hintergrunds.
// Schrittweise Migration: Falls ein Legacy-Framebuffer angehängt ist, wird dieser
// weiter unterstützt; bevorzugt wird jedoch der interne Framebuffer der Klasse.
class BackgroundArt {
public:
  void setUrl(const String& url) { url_ = url; }

  // Startet einen neuen Hintergrundjob (Download+Decode). Idempotent, wenn bereits busy oder URL unverändert.
  void start();

  // Treibt die internen Jobs (z. B. Start des Decode-Workers, wenn Datei fertig)
  void tick();

  // Blit eines nicht-überlappenden Strips auf das Display (24px), mit Top/Bottom-Reserve
  void blitStep(ui_gfx::Display& disp);

  // Blit einen vertikalen Bereich [y0..y1] aus dem aktuellen FB zurück auf das Display
  void blitRegion(ui_gfx::Display& disp, int y0, int y1);

  // Signal, ob seit dem letzten Abfragen geblittet wurde (wird beim Abfragen zurückgesetzt)
  bool consumeDidBlit();

  // Zugriff für Decoder-Callbacks
  inline uint16_t* fbRaw() { return fb_; }

  // Legacy-Unterstützung: an bestehende globale States anhängen (wird bei Migration entfernt)
  void attachLegacy(uint16_t** fb_ptr, volatile bool* ready_ptr, int* blit_row_ptr) {
    legacy_fb_ptr_ = fb_ptr; legacy_ready_ptr_ = ready_ptr; legacy_blit_row_ptr_ = blit_row_ptr;
  }

  // State
  bool ready() const { return (fb_ && ready_) || (legacy_fb_ptr_ && *legacy_fb_ptr_ && legacy_ready_ptr_ && *legacy_ready_ptr_); }
  bool busy() const { return job_busy_ || decode_busy_; }

private:
  // Konfiguration/Quelle
  String url_;
  String last_started_url_ = "";
  const char* path_ = "/album.bin";

  // Interner Framebuffer und Status
  uint16_t* fb_ = nullptr;
  bool ready_ = false;
  bool decode_busy_ = false;
  bool job_busy_ = false;
  bool file_ready_ = false;
  int blit_row_ = 0;
  volatile bool did_blit_ = false;

  // Optional: Legacy-States (solange Migration läuft)
  uint16_t** legacy_fb_ptr_ = nullptr;
  volatile bool* legacy_ready_ptr_ = nullptr;
  int* legacy_blit_row_ptr_ = nullptr;
};

} // namespace albumart

