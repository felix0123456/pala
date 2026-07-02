# PalaOne_Heltec (E-Reader Firmware v1.7.3)

## 1. Project Goals & Outline

_Ziele des Projekts sowie ein grober Überblick über den Aufbau._

- **Ziel:** Firmware für den PalaOne E-Reader auf Basis des Heltec Wireless Paper V1.2 (ESP32-S3). Diese Version (v1.7.3) führt eine To-Do/Checklist-Funktion, Google Calendar Integration mit NTP-Zeitsynchronisation, einen strukturellen Kapitel-Parser für E-Books, einen vergrößerten Schach-Bildschirm mit flexibler Auswahlsteuerung, erweiterte Systemstatistiken (Speichernutzung) und optimierte Navigation (Universelle Triple-Click-Back-Funktion) ein.
- **Strukturübersicht:** Das Projekt enthält Firmware-Versionen für das Heltec Wireless Paper. Die neueste Version (v1.7.3) befindet sich in diesem Ordner (`PalaOne1_7_3_HeltecV1_2`).

## 2. Software Structure
_Dokumentation der Software-Architektur und der verwendeten Dateien._

- **Python:** 
  - [spotify_setup.py](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/spotify_setup.py): Skript im Hauptordner zur automatischen Spotify-OAuth-Authentifizierung und Konfiguration des E-Readers.
- **C++:** 
  - [PalaOne1_7_3_HeltecV1_2.ino](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_3_HeltecV1_2/PalaOne1_7_3_HeltecV1_2.ino): Haupt-Sketch für die E-Reader-Firmware v1.7.3.
  - [web_spa.h](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_3_HeltecV1_2/web_spa.h): Web-Dashboard HTML/JS Ressourcen in PROGMEM mit Unterstützung für Chess Elo, Kalender-URL und Storage-Visualisierung.
  - [pala_one_sleep_black_icon_v4.h](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_3_HeltecV1_2/pala_one_sleep_black_icon_v4.h): Header mit Bildressourcen für den Sleep-Modus.
  - [partitions.csv](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_3_HeltecV1_2/partitions.csv): Partitionstabelle zur Zuweisung des Speichers für App-Code und LittleFS.

## 3. Hardware Structure & Details

_Detaillierte Angaben zur verwendeten Hardware, dem Mikrocontroller und der Verkabelung._

### 3.1 Hardware Infos

- **Microcontroller Type:** ESP32-S3 (Heltec Wireless Paper V1.2)

### 3.2 Flashing Information

- **COM Port:** COM12
- **Flashing Instructions:** Flash über das PowerShell-Skript `flash.ps1` (mit Timeout-Timer) oder direkt mit `arduino-cli`.
  
  **Empfohlene Methode (mit PowerShell-Skript und Flash-Timer):**
  ```powershell
  # Kompilieren und Flashen
  .\flash.ps1 -Port COM12 -TimeoutSec 60
  
  # Nur Kompilieren
  .\flash.ps1 -CompileOnly
  ```

### 3.3 Connections

#### Physical In- and Outputs

- **Available:** E-Ink-Display-Pins, Taster, USB-C.
- **Used:** GPIOs zur Ansteuerung des e-Paper-Bildschirms sowie ein Taster an GPIO 0 zur Steuerung (1x Klick, 2x Klick, Long Press).

#### Communication In- and Outputs

- **Available Protocols:** SPI (Display), Wi-Fi (Client & AP Web-Server), BLE 5.0 (Ebook Upload), USB-Serial (Debugging & Flashing).
