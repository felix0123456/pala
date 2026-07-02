# PalaOne_Heltec (E-Reader Firmware)

## 1. Project Goals & Outline

_Ziele des Projekts sowie ein grober Überblick über den Aufbau._

- **Ziel:** Firmware für den PalaOne E-Reader auf Basis des Heltec Wireless Paper V1.2 (ESP32-S3).
- **Strukturübersicht:** Das Projekt enthält Firmware-Versionen für das Heltec Wireless Paper. Die neueste Version (v1.7.0) befindet sich im Ordner `PalaOne1_7_HeltecV1_2`.

## 2. Software Structure
_Dokumentation der Software-Architektur und der verwendeten Dateien._
- **Python (Setup Tool):**
  - [spotify_setup.py](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/spotify_setup.py): Ein konsolenbasiertes Setup-Skript, das die Spotify-OAuth-Authentifizierung automatisiert und die Zugangsdaten direkt an den E-Reader überträgt.
- **C++ (v1.7.0):** 
  - [PalaOne1_7_HeltecV1_2.ino](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_HeltecV1_2/PalaOne1_7_HeltecV1_2.ino): Haupt-Sketch für die E-Reader-Firmware v1.7.0. Unterstützt:
    - **Wi-Fi Station (STA) Modus**: Verbindet sich automatisch mit dem stärksten erreichbaren Heim-WLAN (unterstützt bis zu 5 saved WiFi-Profile).
    - **Backup Access Point (AP) Modus**: Fällt bei Verbindungsfehler zurück auf ein direktes Wi-Fi (`PALA-XXXXXX` / `palaread`).
    - **mDNS Responder**: Erreichbarkeit unter `http://pala.local`.
    - **Web Bluetooth (BLE) Upload**: Zuverlässige BLE-Übertragung mit automatischer Werbungs-Reinitialisierung nach Verbindungsabbrüchen.
    - **Premium Explorer Web UI**: Windows-Explorer-ähnliches Ordnerbaum- und Dateisystem, AJAX-Drag-and-Drop-Upload, Gutenberg-Downloader mit Proxy-Fallbacks, progress Remote-Slider und Text-Quick-Editor.
    - **Spotify screensaver/app**: Zeigt Albumcover, Songtitel, Interpret und Wiedergabestatus auf dem E-Ink-Display an, mit Tastensteuerung (Wiedergabe/Pause, Titel überspringen/vorheriger, Zurück zur Bibliothek).
    - **Night Mode**: Dynamische Pixelinversion auf dem Display.
  - [pala_one_sleep_black_icon_v4.h](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_HeltecV1_2/pala_one_sleep_black_icon_v4.h): Header mit Bildressourcen für den Sleep-Modus.
  - [partitions.csv](file:///c:/Users/felix/Downloads/PalaOne1_5_HeltecV1_2/PalaOne1_7_HeltecV1_2/partitions.csv): Partitionstabelle zur Zuweisung des Speichers für App-Code und LittleFS.

## 3. Hardware Structure & Details

_Hardware, Mikrocontroller und Verkabelung._

### 3.1 Hardware Infos

- **Microcontroller Type:** ESP32-S3 (Heltec Wireless Paper V1.2)

### 3.2 Flashing Information

- **COM Port:** COM12
- **Flashing Instructions:** Flash über `arduino-cli` oder Arduino IDE mit dem Board `esp32:esp32:heltec_wireless_paper` bei einer Upload-Geschwindigkeit von 115200 Baud.
  ```bash
  # Compile v1.7.0
  arduino-cli compile --fqbn esp32:esp32:heltec_wireless_paper PalaOne1_7_HeltecV1_2\PalaOne1_7_HeltecV1_2.ino
  
  # Flash v1.7.0
  arduino-cli upload -p COM12 -b esp32:esp32:heltec_wireless_paper:UploadSpeed=115200 PalaOne1_7_HeltecV1_2\PalaOne1_7_HeltecV1_2.ino
  ```

### 3.3 Connections

#### Physical In- and Outputs

- **Available:** E-Ink-Display-Pins, Taster, USB-C.
- **Used:** GPIOs zur Ansteuerung des e-Paper-Bildschirms sowie ein Taster an GPIO 0 zur Steuerung.

#### Communication In- and Outputs

- **Available Protocols:** SPI (Display), Wi-Fi (Client & AP Web-Server), BLE 5.0 (Ebook Upload), USB-Serial (Debugging & Flashing).
