# SonosRotaryController v1.99 Release Notes

## 🚨 **KRITISCHE BUGFIXES & STABILITÄT**

Version 1.99 fokussiert sich auf **Stabilität und Zuverlässigkeit** nach kritischen UI-Problemen in vorherigen Versionen.

### **🔧 Kritische Reparaturen**

#### **1. Touch-Interface System-Freeze behoben**
- **Problem:** Touch-Feedback-System verursachte komplettes System-Einfrieren
- **Symptome:** Play-Button-Pfeil verschwand, Volume-Button zeigte grünen Kreis, dann Freeze
- **Lösung:** Touch-Feedback-System sicher deaktiviert, alle Touch-Events funktionieren wieder
- **Status:** ✅ **VOLLSTÄNDIG BEHOBEN**

#### **2. Album Art Clipping nach Menu repariert**
- **Problem:** Nach Config-Menu-Rückkehr war 1/3 des Album Arts abgeschnitten
- **Ursache:** Progressive Blitting überschrieb `forceFullRedraw()` zu früh
- **Lösung:** 
  - Pause-Dauer von 3s auf 5s erhöht
  - `blit_row_ = 480` (Ende) nach `forceFullRedraw()` gesetzt
  - Progressive Blitting stoppt nach vollständigem Redraw
- **Status:** ✅ **VOLLSTÄNDIG BEHOBEN**

#### **3. UI-Element Responsiveness wiederhergestellt**
- **Problem:** Touch-Events reagierten nicht oder verzögert
- **Lösung:** Alle `start_touch_feedback()` Aufrufe entfernt, die Konflikte verursachten
- **Ergebnis:** Sofortige, zuverlässige Touch-Reaktion
- **Status:** ✅ **VOLLSTÄNDIG BEHOBEN**

### **⚡ Performance-Optimierungen**

#### **HTTP-Timeout-Standardisierung**
```cpp
static constexpr int HTTP_TIMEOUT_QUICK = 1200;   // Quick operations
static constexpr int HTTP_TIMEOUT_NORMAL = 3000;  // Normal SOAP operations
static constexpr int HTTP_TIMEOUT_LONG = 8000;    // Long operations
```

#### **Magic Numbers eliminiert**
```cpp
static constexpr int DISPLAY_WIDTH = 480;
static constexpr int DISPLAY_HEIGHT = 480;
static constexpr int SONOS_POLL_INTERVAL = 1000;
static constexpr int MEMORY_LOG_INTERVAL = 5000;
```

#### **Progressive Blitting optimiert**
- Intelligente Pause-Mechanik verhindert UI-Konflikte
- Silent-Modus reduziert Debug-Spam
- Robuste End-State-Erkennung

### **🧹 Code-Bereinigung**

#### **Legacy Album Art System entfernt**
- **Gespart:** ~1.5MB PSRAM, ~300 Zeilen Code
- **Entfernt:** Doppelte Implementierungen, veraltete Fallbacks
- **Ergebnis:** Saubere, wartbare Codebase

#### **Logging-System vereinheitlicht**
- **Entfernt:** Doppelte `logging.h`
- **Behalten:** Einheitlich `base/Log.h`
- **Ergebnis:** Konsistente Logging-Infrastruktur

#### **Include-Struktur optimiert**
- Redundante Includes entfernt
- Abhängigkeiten bereinigt
- Kompilierzeit verbessert

### **📊 System-Performance**

#### **Memory-Management**
```
Heap:  7.2MB frei (stabil)
PSRAM: 6.9MB frei (optimiert)
Flash: 19.7% (1.29MB von 6.55MB)
RAM:   16.2% (53KB von 327KB)
```

#### **Album Art System**
```
Decode-Zeit: ~500ms (sehr schnell)
Progressive Blitting: Gestoppt nach forceFullRedraw()
Memory-Overhead: Eliminiert
```

#### **Sonos-Integration**
```
Polling-Intervall: 1000ms (optimiert)
HTTP-Timeouts: Standardisiert
Room-Discovery: 6 Räume stabil
```

### **🎯 Erreichte Verbesserungen**

1. **✅ System-Stabilität:** Keine Freezes oder Crashes mehr
2. **✅ Touch-Responsiveness:** Sofortige, zuverlässige Reaktion
3. **✅ Album Art:** Vollständige Anzeige nach Menu-Navigation
4. **✅ Memory-Effizienz:** Stabile Speicher-Nutzung ohne Leaks
5. **✅ Code-Qualität:** Wartbare, saubere Implementierung
6. **✅ Performance:** Optimierte HTTP-Timeouts und Polling
7. **✅ Debugging:** Reduzierter Log-Spam, fokussierte Meldungen

### **🔄 Bekannte Einschränkungen**

- **Touch-Feedback:** Temporär deaktiviert bis robuste Implementierung verfügbar
- **Button-Animationen:** Zurückgestellt zugunsten Stabilität
- **Visual Effects:** Minimiert um UI-Konflikte zu vermeiden

### **🚀 Nächste Schritte**

Version 1.99 legt den Grundstein für zukünftige Features durch:
- Stabile, getestete Basis-Funktionalität
- Saubere Code-Architektur
- Robuste Memory-Management
- Optimierte Performance-Parameter

### **📋 Upgrade-Empfehlung**

**DRINGEND EMPFOHLEN** für alle Benutzer vorheriger Versionen:
- Behebt kritische System-Freezes
- Stellt vollständige UI-Funktionalität wieder her
- Verbessert Stabilität und Performance erheblich

---

**Version 1.99 - Stabilität first! 🚀**
