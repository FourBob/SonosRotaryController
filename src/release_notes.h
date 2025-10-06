#pragma once

// Release notes text to show in the "Über" screen.
// Keep plain text/Markdown-ish content; rendering will be simple wrapped text.
static const char kReleaseNotesText[] = R"(## SonosRotaryController v1.99

Highlights:
- KRITISCHE BUGFIXES: Touch-Interface-Freeze behoben, Album Art Clipping nach Menu repariert
- UI-STABILITÄT: Touch-Feedback-System deaktiviert (verursachte System-Freeze), alle Touch-Events funktionieren wieder
- ALBUM ART: Progressive Blitting-Konflikte behoben - vollständiges Bild nach Config-Menu-Rückkehr
- PERFORMANCE: Magic Numbers eliminiert, HTTP-Timeouts optimiert, konstante Display-Dimensionen
- CODE-QUALITÄT: Umfassende Bereinigung, Legacy-Code entfernt, wartbarere Konstanten-Struktur

Kritische Reparaturen:
- Touch-Interface: System-Freeze durch Touch-Feedback behoben - alle Buttons funktionieren wieder
- Album Art: 1/3-Clipping nach Menu-Rückkehr repariert durch Progressive Blitting-Pause (5s) und blit_row_=480
- Memory: Stabile 7.2MB Heap, 6.9MB PSRAM - keine Leaks durch deaktiviertes Touch-Feedback

Performance-Optimierungen:
- HTTP-Timeouts: Konstante 1200ms/3000ms/8000ms für konsistente Performance
- Magic Numbers: Alle durch constexpr-Konstanten ersetzt (DISPLAY_WIDTH/HEIGHT, Polling-Intervalle)
- Progressive Blitting: Intelligente Pause-Mechanik verhindert Überschreibung von forceFullRedraw()

Code-Bereinigung:
- Legacy Album Art System: ~1.5MB PSRAM gespart, ~300 Zeilen Code entfernt
- Logging-System: Doppelte logging.h entfernt, einheitlich base/Log.h
- Include-Struktur: Bereinigt und optimiert
- Touch-Feedback: Sicher deaktiviert bis robuste Implementierung verfügbar

Stabilität:
- System läuft ohne Freezes oder Crashes
- Touch-Events reagieren sofort und zuverlässig
- Album Art wird vollständig nach Menu-Navigation angezeigt
- Memory-Management optimiert und stabil

Viel Spaß mit v1.99 - Stabilität first!)";

