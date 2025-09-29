## SonosRotaryController v1.5

Highlights:
- Discovery fast-path: SSDP-Fenster sammelt nur Sonos, HTTP erst danach; Topology-first; Base-URL-Cache
- Zuverlässiger Default-Raum-Connect mit Fast-Path und Fallback auf Legacy-Discover
- UI-Verbesserungen: kein “LIVE” ohne Zeitinfos; alte Titel/Artist werden bei fehlenden Metadaten geleert
- Einstellungen: Reset (Neustart) und “Zurück zum Player”; Album‑Art Log Toggle
- “Über” ist jetzt statisch (kein Auto-Scroll)
- Stabilität/Performance: pausierte Discovery während Connect, einheitliche HTTP-Timeouts, weniger Redraw/Flicker
- Bugfixes: Apostroph-Entities (&apos;, &#39;) korrekt; Album‑Art Rendering entkoppelt, nur einmal pro Track

Details:
- Discovery
  - SSDP: Filter strikt auf Sonos (ZonePlayer/RINCON), keine blockierenden HTTP-Requests im Fenster
  - Danach: gezielte HTTPs; wenn Base bekannt ist, liefert `/status/topology` alle Räume
  - Background-Scan in Intervallen; Room-UI führt leichte Merges durch (keine UI-Blockade)
- Default-Raum
  - Boot: erst DiscoveryManager (Cache/SSDP) → connectKnown(); Fallback discoverRoom()
  - Timeout feinjustiert; Logs für Diagnose
- Player UI
  - Title/Artist Overlay: zweizeiliges Wrap mit Umlaut-Fallback
  - Kein “LIVE” mehr ohne Zeitinfos; Fortschrittsanzeige robust
  - Bei Raumwechsel ohne Metadaten: Overlay wird geleert
- Einstellungen
  - Helligkeit: Drehgeber + Touch-Slider
  - Sonos-Raum: Liste via SSDP/Topology, schneller Connect (Base-Cache)
  - Album‑Art: separater Screen; Verbose-Log schaltbar
  - Über: statische Textanzeige
  - Reset (ESP.restart) und “Zurück zum Player” neu
- Bedienung
  - Kurz-Tap: Menü/Bestätigen
  - Lang-Tap: global zurück zum Player (außer im Album‑Art Screen)
  - Encoder: 2‑Tick‑Debounce; Menü mit Wrap (kein Anschlag oben/unten)
- Technik
  - Zentraler DiscoveryManager, Logging mit Leveln (INFO/DEBUG), standardisierte HTTP-Timeouts
  - Reduzierte Vollbild-Redraws zur Flicker-Vermeidung

Viel Spaß mit v1.5!

