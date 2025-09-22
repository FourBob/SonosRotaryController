# Sonos Rotary Controller (ESP32‑S3 MaTouch 2.1" Round)

Deutschsprachige Firmware für den MaTouch ESP32‑S3 Rotary IPS (2.1" rund, 480×480, ST7701S) als kompakter Sonos‑Controller mit Drehgeber, Touch und einfachem UI.

## Features
- Sonos Steuerung: Play/Pause, Vor/Zurück, Lautstärke (Drehgeber), Raumwahl
- Album‑Art Anzeige (JPEG/PNG) mit performanter Darstellung
- Titel/Artist Overlay mit sauberem Zeilenumbruch (Umlaute‑Fallback)
- Zeit/Progress Anzeige; wenn keine Zeitinfos: links unten leer (kein „LIVE“)
- Einstellungsmenü mit:
  - Helligkeit (Drehgeber + Touch‑Slider)
  - Sonos Raum wählen (Liste via SSDP/Topology)
  - Album‑Art Ansicht (optional, ggf. Test/Debug)
  - Über (statischer Text, kein Scrollen)
  - Album‑Art Log: Ein/Aus (Debug)
  - Reset (Neustart)
  - Zurück zum Player
- Lange Taste: aus jedem Screen zurück zum Player (außer Album‑Art)
- Stabil und flott: zentraler Discovery‑Manager, einheitliche HTTP‑Timeouts, non‑blocking UI

## Hardware
- MaTouch ESP32‑S3 Rotary IPS 2.1" (ST7701S, 480×480)
- Touch: CST826 (I2C)
- Drehgeber + Taster (am Board)

## Software/Build
- PlatformIO ist bereits konfiguriert (Arduino‑Framework)
- Ziel‑Environment: `matouch_esp32s3`

### Bauen & Flashen
<augment_code_snippet mode="EXCERPT">
````bash
# Bauen
~/.platformio/penv/bin/platformio run -e matouch_esp32s3

# Flashen (USB CDC/JTAG, macOS Beispiel‑Port)
~/.platformio/penv/bin/platformio run -e matouch_esp32s3 -t upload \
  --upload-port /dev/cu.usbmodem2101
````
</augment_code_snippet>

### Serieller Monitor (Logs)
<augment_code_snippet mode="EXCERPT">
````bash
~/.platformio/penv/bin/platformio device monitor \
  -p /dev/cu.usbmodem2101 -b 115200
````
</augment_code_snippet>

## Konfiguration
- WLAN und Default‑Raum in `src/secrets.h` setzen:
<augment_code_snippet path="src/secrets.h" mode="EXCERPT">
````cpp
// Beispiel
#define WIFI_SSID "IhrSSID"
#define WIFI_PASS "IhrPasswort"
#define DEFAULT_SONOS_ROOM "Elternschlafzimmer"
````
</augment_code_snippet>
- Optional: Sprache/Labels in `src/app_locale.h`

Beim Boot wird zuerst WLAN verbunden. Danach versucht die Firmware, den `DEFAULT_SONOS_ROOM` schnell zu finden (DiscoveryManager Fast‑Path mit Cache/SSDP; Fallback auf Legacy‑Discover).

## Bedienung
- Drehgeber: Lautstärke im Player; Navigation in Menüs
- Taster kurz: öffnet Einstellungen bzw. bestätigt Menüpunkt
- Taster lang: aus (fast) jedem Screen zurück zum Player
- Touch‑Zonen im Player:
  - Mitte: Play/Pause
  - Links/Rechts: Vor/Zurück
- Brightness‑Screen: Helligkeit per Drehgeber oder Touch‑Slider einstellen

## Einstellungen (Menü)
Reihenfolge der Punkte:
1. Helligkeit
2. Sonos Raum
3. Album‑Art
4. Über (statischer Text)
5. Album‑Art Log (An/Aus)
6. Reset (ESP.restart)
7. Zurück zum Player

Hinweise:
- „Zurück zum Player“ springt ohne Umwege in den Player‑Screen.
- „Reset“ startet das Gerät neu.

## Sonos & Discovery (Kurz erklärt)
- Zentraler DiscoveryManager:
  - Kurzer SSDP‑Burst (gefiltert nur Sonos) – während des Fensters keine HTTP‑Anfragen
  - Danach nur wenige gezielte HTTP‑Requests; bekannte „Base URLs“ werden gecached
  - „Topology‑first“: Wenn eine Base bekannt ist, liefert `/status/topology` alle Räume
- Default‑Connect beim Boot:
  - Erst Fast‑Path via Cache/SSDP → `connectKnown()`
  - Fallback: Legacy `discoverRoom()` mit etwas erweitertem Timeout
- Bei Raumwechsel ohne Metadaten werden alte Titel/Artist zuverlässig geleert

## Performance & Stabilität
- Einheitliche HTTP‑Timeouts (~800–1200 ms)
- Pausierter Discovery während Connects (kein Parallel‑Scan)
- UI aktualisiert gezielt (keine Vollbild‑Flicker), Titel/Artist nur bei Änderungen
- Logging mit Leveln (INFO/DEBUG) – siehe `src/logging.h`

## Fehlerbehebung
- Default‑Raum wird beim Boot nicht gefunden:
  - Prüfen, ob `DEFAULT_SONOS_ROOM` exakt dem Sonos‑Raumnamen entspricht
  - Warten bis WLAN „CONNECTED“ (siehe Logs), dann startet Discovery
  - Raum manuell im Menü „Sonos Raum“ wählen; Base wird für nächste Male gecached
- Kein Flash möglich (Port busy): seriellen Monitor schließen, dann erneut flashen
- Keine Zeit/Progress: Bei Radios/Streams ist `relTime`/`duration` oft „NOT_IMPLEMENTED“ – Anzeige bleibt links leer (kein „LIVE“)

## Ordnerstruktur
<augment_code_snippet mode="EXCERPT">
````text
src/
  main.cpp          # UI, Screens, Loop, Setup
  sonos.{h,cpp}     # Sonos Client (SOAP, Polling, Steuerung)
  discovery.{h,cpp} # DiscoveryManager (SSDP/Topology, Cache)
  touch.{h,cpp}     # Touch-Layer
  logging.h         # Log-Level Makros
  app_locale.h      # Sprach-Labels
  secrets.h         # WLAN/Default-Raum (ausfüllen)
platformio.ini      # PIO Konfiguration (env: matouch_esp32s3)
````
</augment_code_snippet>

## Lizenz
Ohne explizite Lizenzangabe – bitte vor Weitergabe/Veröffentlichung nachfragen.

