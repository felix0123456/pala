# Pala Cloud & Firmware

## 1. Project Goals & Outline

_Das Pala Projekt kombiniert eine lokale E-Reader Firmware (ESP32) mit einer Cloud-Plattform (Pala Cloud Hub) zur nahtlosen Verwaltung und Synchronisation von E-Books, Kalendern und Spotify._

- **Ziel:** Eine umfassende, autarke E-Reader-Erfahrung mit minimalem manuellem Verwaltungsaufwand, OTA-Updates und Cloud-gestütztem E-Book-Fetching bereitzustellen.
- **Strukturübersicht:** Das Projekt gliedert sich in zwei Hauptkomponenten: Die ESP32 C++ Firmware (im Ordner `firmware/current/`) und das Python FastAPI Backend für die Cloud-Plattform. Alle älteren Firmware-Iterationen sind unter `firmware/legacy/` archiviert.

## 2. Software Structure

_Die Architektur besteht aus einem Server-Backend und der Mikrocontroller-Software._

- **Python (Cloud Hub):** Die Dateien im Hauptverzeichnis (z.B. `app.py`) bilden den Pala Cloud Hub, gehostet auf Coolify. 
  - **OTA Updates:** Das Backend stellt unter `/api/firmware/check` eine Versionierungs-Prüfung bereit und liefert unter `/api/firmware/latest.bin` kompilierte Binärdateien aus, welche von den Geräten per HTTP-Stream geflasht werden. Die aktuellen Binaries müssen im Ordner `firmware/build/` bereitgestellt werden.
  - **Auto-Sync:** Über `/api/sync` fragt der E-Reader automatisch ab, ob neue Bücher heruntergeladen werden sollen.
- **C++ (Firmware):** Die Firmware in `firmware/current/` steuert das Display, verbindet sich per WiFi mit der Cloud und holt über `HTTPUpdate` Over-The-Air (OTA) Updates ab. Dies kann manuell im E-Reader-Menü unter `Settings -> Cloud Sync & Update` ausgelöst werden.

## 3. Hardware Structure & Details

_Details zur zugrundeliegenden E-Reader Hardware._

### 3.1 Hardware Infos

- **Microcontroller Type:** ESP32 (Heltec Wireless Paper V1.2 / V1.1)

### 3.2 Flashing Information

- **COM Port:** Typischerweise `COM12` (kann je nach System variieren).
- **Flashing Instructions:** Nutze das bereitgestellte PowerShell-Skript (`flash.ps1`) oder die Arduino IDE. Bei Problemen mit dem Bootloader-Modus den "Boot"-Knopf am Heltec-Board gedrückt halten und kurz "Reset" drücken. Künftig werden Updates vorrangig per OTA verteilt.

### 3.3 Connections

#### Physical In- and Outputs

- **Available:** GPIO-Pins des ESP32, USB-C Port, Onboard-Buttons.
- **Used:** Interne SPI-Verbindung zum E-Paper-Display, I2C/SPI für Peripherie (wie Buttons/Batteriemanagement).

#### Communication In- and Outputs

- **Available Protocols:** WiFi, Bluetooth (WebBLE).
