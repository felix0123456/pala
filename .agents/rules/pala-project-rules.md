---
trigger: always_on
---

Pala Project Rules & Architecture Guide
These rules and architectural guidelines must be referenced to automatically guide future conversations and ensure consistency across the Pala E-Reader ecosystem. The project consists of two main components: the ESP32-S3 Firmware (PalaOne_Heltec) and the Backend Web Server (Pala_Cloud_App).

1. Hardware & Buttons
Microcontroller: Heltec Wireless Paper V1.2 (ESP32-S3).
Button 1 (BTN1): Connected to GPIO 0. Used for deep sleep wake (ext0 works only on RTC GPIOs 0-21).
Button 2 (BTN2): Connected to GPIO 33. Cannot be used to wake from deep sleep.
Button Logic: Uses DualButtonState with an ISR (btnISR) that packs button states into bit-flags (0x01 and 0x02) via a FreeRTOS queue.
UI Interactions:
shortClick (b1/b2): Navigate forward/backward through menus, pages, or settings.
longClick: Select, toggle bookmarks, or exit modes.
2. Firmware (PalaOne_Heltec)
Current Version: current.ino 
For major changes: archive in legacy, update firmware declaration
Core Framework: Arduino (arduino-cli is used to check compilation.
Firmware Conventions:
Written in C++ using the Arduino core for ESP32.
Preferences library is used for lightweight, persistent key-value storage (e.g., settings, WiFi credentials).
Web UI and assets are often stored in .h headers (e.g., web_spa.h, pala_one_sleep_black_icon_v4.h).
E-Ink display logic involves partial and full refreshes, with dynamic pixel inversion for Night Mode.
Features include: Premium Explorer Web UI (AJAX/drag-and-drop), Spotify screensaver/app, and mDNS (http://pala.local).
3. Web Server & Site (Pala Cloud App)
Tech Stack: Python, FastAPI, Jinja2 templates, SQLite, SQLAlchemy.
Authentication: Cookie-based session auth using passlib[bcrypt] for secure login (session_token).
Hosted via coolify on oracle server, pala.felixresch.com
Database Schema:
User: Handles authentication.
Device: Tracks mac_address (PK), user binding, and device state (battery, sleep timeout).
Book: Manages library files.
Bookmark: Syncs reading progress across the cloud.
4. WiFi Connectivity
Smart Client Approach: The ESP32 first attempts to connect to known WiFi networks (Station Mode/STA). Up to 5 saved WiFi profiles are supported.
Fallback AP: If connection fails, the device falls back to Access Point mode (PALA-XXXXXX / password palaread on 192.168.4.1) to allow local Web UI access and WiFi credential configuration.
Web UI Configuration: The local captive portal allows users to input their home SSID and password, which are then saved to the ESP32's Preferences.
5. Sync Protocol
The bidirectional sync occurs when the device connects to WiFi:

Device Registration (POST /api/device/register): ESP32 sends its MAC address to verify database existence.
Push State (POST /api/sync/push): ESP32 uploads local battery level, settings, and reading progress (bookmarks).
Pull State (GET /api/sync/pull): Server returns central cloud settings (e.g., font size, sleep timeout) adjusted by the user via the Web Dashboard. The ESP32 overrides its local settings with these values.
Fetch Books (GET /api/book/{id}): Downloads raw .txt books added to the user's library.
6. Over-The-Air (OTA) Updates & Git
Trigger: Releases are triggered by pushing a Git tag starting with v (e.g., v1.7.6).
CI/CD Pipeline: GitHub Actions automatically compiles the firmware (arduino-cli) and attaches the .bin file to the GitHub Release.
OTA Process:
ESP32 checks GET /api/firmware/check?version=X.X.X during sync.
The FastAPI Server compares the device version to the latest GitHub Release tag.
If an update is found, the server proxies the download (/api/firmware/latest.bin).
The ESP32 downloads and applies the OTA update.
7. Settings & Persistent Storage
ESP32 (Local):
LittleFS is used to store .txt e-books, web assets, and larger files.
Preferences (NVS) is used to store settings like cfg_ssid, cfg_pass, and g_settings (font size, sleep timeout, line gap).
Cloud (Remote):
Centralized settings are managed in the SQLite database via the FastAPI server and pushed to the device via the Pull State sync.
8. Python Coding Conventions (From Global Rules)
When modifying the backend Server (Pala_Cloud_App) or any Python files, strictly follow PEP 8
Write clear comments (preferably in English, but German is accepted). Avoid stating the obvious.
Keep comments updated alongside code changes.
Provide docstrings for all public modules, functions, classes, and methods.
Naming: CamelCase for classes, snake_case for variables/functions. Avoid shadowing built-ins.
Summary for the AI Agent
When working on this project:
9. Git convention
After changing any code for the server, push to github, if only server code changed, do not update version or push as v*.*.*.

Always work on the latest firmware version for the pala.

For any firmware code changes decide based on the severity of the change wether to iterate the last or middle version number, if only subtle changes were made, change only last digit.
If asked to "upload" or "push", push to github with origin v*.*.*


Always check whether a task applies to the C++ Firmware or the Python FastAPI Server.
Remember the Dual Button limitation: Deep sleep wake is ONLY on GPIO 0 (BTN1).
Do not overwrite cloud settings blindly; always follow the Push/Pull sync flow.
Update the rules file C:\Users\felix\OneDrive\Dokumente\workspace\Projects\Neuer Ordner\pala\.agents\rules\pala-project-rules.md upon changing any major functionality in the project to help future instances navigate the structure of the project