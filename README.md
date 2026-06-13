# Tesla Ambiente

> iOS App + ESP32 BLE-Controller für Tesla Umgebungsbeleuchtung

Tesla Ambiente ist ein Open-Source-Projekt zur vollständigen Steuerung von WS2812B-LED-Streifen in einem Tesla-Fahrzeug. Das System besteht aus einer iOS-App (Swift/SwiftUI) und einem ESP32-basierten Controller-Netzwerk, das via BLE, ESP-NOW und CAN-Bus kommuniziert.

---

## Features

### iOS App
- **BLE-Steuerung** — Echtzeit-Verbindung zum ESP32 Master via CoreBluetooth
- **20 LED-Effekte** — Static, Breathing, Rainbow, Fire, Police, MeteorRain und mehr
- **Zonen-Steuerung** — Individuelle Konfiguration jeder Fahrzeugtür + Dashboard
- **Fahrzeugstatus** — Live-Anzeige von Blinker, SOC, Gang, Türen, Autopilot, Totwinkel
- **Dashboard-Features** — Autopilot-LED, Totwinkel-Warnung, Blinker-LED, Ladevorgänge
- **Presets** — 5 speicherbare Beleuchtungsprofile
- **OTA Updates** — Firmware-Upload via BLE direkt aus der App (Developer-Bereich)
- **Dark Mode** — Standard-Darstellung mit Glassmorphism-Design und SF Symbols
- **MVVM-Architektur** — Sauber strukturiert mit ObservableObject, Combine, UserDefaults

### Hardware-System
- **ESP32 Master** — WiFi AP + ESP-NOW + BLE, Web-Overlay, OTA
- **ESP32-C6 CAN-Bridge** — Dual TWAI (VehicleBus + ChassisBus), 122 LEDs (Dashboard)
- **4× ESP32 Door Slaves** — Je 130 LEDs, Tür-spezifische Effekte, OTA-fähig
- **ESP-NOW Mesh** — Latenzarme Kommunikation auf Kanal 6 (Broadcast)
- **CAN-Bus Decoding** — Tesla-spezifische CAN-IDs für Fahrzeugdaten

---

## Voraussetzungen

### Hardware
- ESP32 (Master) — mit ausreichend RAM für WiFi + BLE + ESP-NOW
- ESP32-C6 (CAN-Bridge/Dashboard) — Dual TWAI, 122 WS2812B LEDs
- 4× ESP32 (Door Slaves) — je 130 WS2812B LEDs
- WS2812B LED-Streifen (empfohlen: 60 LED/m oder 144 LED/m)
- Geeignete Spannungsversorgung (5V für LEDs, 3.3V für ESP32)

### Software
- Arduino IDE 2.x oder PlatformIO
- **ESP32 Board Package** ≥ 3.0 (ESP-IDF v5)
- Arduino-Bibliotheken:
  - `NimBLE-Arduino` ≥ 1.4.x
  - `Adafruit NeoPixel`
  - `ArduinoJson` (optional, für Debug)

### iOS
- Xcode 15 oder neuer
- iOS 17.0+ Deployment Target
- Echtes iPhone (kein Simulator für BLE)
- Apple Developer Account (für Device-Deployment)

---

## Einrichtung

### 1. Arduino-Bibliotheken installieren

Im Arduino IDE (Tools → Bibliotheken verwalten):
```
NimBLE-Arduino       (h2zero)
Adafruit NeoPixel    (Adafruit)
```

### 2. ESP32-Board-Package

In Arduino IDE → Einstellungen → Zusätzliche Board-Manager-URLs:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Dann: Board-Manager → `esp32 by Espressif Systems` ≥ 3.0 installieren.

### 3. Arduino-Dateien flashen

**Reihenfolge:**
1. Door Slaves zuerst (4×): `TeslaCode/TeslaDoorSlave/TeslaDoorSlave.ino`
   - `DEVICE_SIDE` anpassen: `'F'` (Vorne links), `'G'` (Vorne rechts), `'R'` (Hinten links), `'L'` (Hinten rechts)
2. CAN-Bridge/Dashboard: `TeslaCode/TeslaDashMultiply/TeslaDashMultiply.ino`
3. Master zuletzt: `TeslaCode/Tesla_Ambiente_Master/Tesla_Ambiente_Master.ino`

**Board-Einstellungen für ESP32 Master:**
```
Board:         ESP32 Dev Module
Partition:     Huge APP (3MB No OTA)  ← für BLE + WiFi + ESP-NOW
Flash Size:    4MB
Upload Speed:  921600
```

**Board-Einstellungen für ESP32-C6:**
```
Board:         ESP32-C6 Dev Module
Partition:     Default 4MB with spiffs
```

### 4. iOS App in Xcode öffnen

```bash
open iOS/TeslaAmbiente.xcodeproj
```

- Team auswählen (Signing & Capabilities)
- Bundle ID ggf. anpassen: `de.teslaambiente.app`
- Auf echtem iPhone deployen (⌘R)

---

## Verbindung herstellen

1. ESP32 Master einschalten (sucht sofort BLE-Clients)
2. iOS App öffnen → **Dashboard** → **"Verbinden"** tippen
3. `Tesla-Ambiente` in der Liste auswählen
4. BLE verbindet sich automatisch

Der Master ist gleichzeitig unter `http://192.168.4.1` im Browser erreichbar (WiFi SSID: `Tesla-Ambiente`, Passwort: `12345678`).

---

## OTA Updates (Firmware-Update)

### Via iOS App (BLE OTA — empfohlen)

1. In der App: **Developer**-Tab (Passwort: `tesla2024`)
2. Ziel-Gerät auswählen (Master oder Slave)
3. Firmware-Datei (`.bin`) über Dateien-Picker auswählen
4. **"Update starten"** → Fortschritt wird angezeigt
5. Gerät startet automatisch neu

> Hinweis: Für Door Slaves und CAN-Bridge leitet der Master den OTA-Befehl via ESP-NOW weiter (`SystemCommand.command = 3`). Danach öffnet das Zielgerät seinen eigenen WiFi-AP.

### Via Web-Interface (Master only)

1. WiFi mit `Tesla-Ambiente` verbinden
2. `http://192.168.4.1/masterota` aufrufen
3. `.bin`-Datei hochladen → Flash wird automatisch durchgeführt

### Firmware-Datei erstellen (Arduino IDE)

```
Sketch → Exportiere kompilierte Binärdatei → .bin-Datei im Sketch-Verzeichnis
```

---

## BLE UUIDs

| Charakteristik | UUID | Eigenschaften |
|---|---|---|
| Service | `4FAFC201-1FB5-459E-8FCC-C5C9C331914B` | — |
| LED Command | `BEB5483E-36E1-4688-B7F5-EA07361B26A8` | Write, Write-NR |
| Vehicle Status | `BEB5483E-36E1-4688-B7F5-EA07361B26A9` | Read, Notify |
| Feature Settings | `BEB5483E-36E1-4688-B7F5-EA07361B26AA` | Read, Write |
| OTA Control | `BEB5483E-36E1-4688-B7F5-EA07361B26AB` | Read, Write, Notify |
| OTA Data | `BEB5483E-36E1-4688-B7F5-EA07361B26AC` | Write, Write-NR |
| Device Info | `BEB5483E-36E1-4688-B7F5-EA07361B26AD` | Read |
| Presets | `BEB5483E-36E1-4688-B7F5-EA07361B26AE` | Read, Write |

---

## Projektstruktur

```
TeslaAmbiente/
├── TeslaCode/
│   ├── Tesla_Ambiente_Master/
│   │   └── Tesla_Ambiente_Master.ino
│   ├── TeslaDashMultiply/
│   │   └── TeslaDashMultiply.ino
│   ├── TeslaDashStandalone/
│   │   └── TeslaDashStandalone.ino
│   └── TeslaDoorSlave/
│       └── TeslaDoorSlave.ino
└── iOS/
    ├── TeslaAmbiente.xcodeproj/
    └── TeslaAmbiente/
        ├── TeslaAmbienteApp.swift
        ├── Info.plist
        ├── Models/
        │   ├── BLEModels.swift
        │   └── AppState.swift
        ├── Services/
        │   └── BLEManager.swift
        ├── ViewModels/
        │   └── MainViewModel.swift
        └── Views/
            ├── ContentView.swift
            ├── Components/GlassCard.swift
            ├── Dashboard/DashboardView.swift
            ├── Dashboard/BLEScannerView.swift
            ├── LEDControl/LEDControlView.swift
            ├── Settings/SettingsView.swift
            └── OTA/OTAView.swift
```

---

## Sicherheitshinweise

- **MAX_LED_BRIGHTNESS_PERCENT = 15** — Helligkeit auf 15% begrenzt
- BLE ohne Pairing-Code — nur im eigenen Fahrzeug betreiben
- OTA-Passwort `tesla2024` in der Produktion ändern
- Nur lesender CAN-Bus-Zugriff — kein Eingriff in Fahrzeugsysteme

---

## Lizenz

MIT License

---

*Projekt von Jesko — gebaut für Tesla Model 3/Y*