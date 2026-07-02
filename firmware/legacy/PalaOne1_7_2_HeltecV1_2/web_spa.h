#ifndef WEB_SPA_H
#define WEB_SPA_H

#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>

// External globals from the main sketch
extern WebServer server;
extern Preferences prefs;

extern WifiProfile g_wifiProfiles[];
extern int g_wifiCount;
extern const int MAX_WIFI_PROFILES;
extern void saveWifiProfiles();
extern void loadWifiProfiles();

extern BookInfo books[];
extern int bookCount;
extern const int MAX_BOOKS;
extern void loadBooks();
extern void ensureBooksDir();

extern char folders[][64];
extern int folderCount;
extern const int MAX_FOLDERS;

extern String currentBookPath;
extern String currentBookKey;
extern File bookFile;
extern int pageIndex;
extern int knownPages;
extern bool eofReached;
extern uint32_t pageOffsets[];

extern void clearCurrentBookState();
extern bool reopenCurrentBookIfNeeded();
extern void renderCurrentPage();
extern String prefKeyForBook(const String& path);
extern uint32_t buildNextOffsetFor(File &f, uint32_t startPos);
extern uint32_t pageOffsetForPage(File &f, int page);
extern String normalizeTypography(const String& in);

extern int g_fontSize;
extern uint32_t g_sleepSecs;
extern int LINE_GAP;
extern bool g_nightMode;
extern bool g_spotifyScreensaver;
extern String g_spotifyClientId;
extern String g_spotifyClientSecret;
extern String g_spotifyRefreshToken;
extern int g_chessElo;

extern void applyFontSize(int sz);
extern void invalidateMetrics();
extern uint32_t SLEEP_AFTER_MS;

extern bool g_uploadOk;
extern String g_uploadError;
extern String g_uploadFinalName;
extern String uploadPending;
extern String uploadPath;
extern File uploadFile;

extern bool g_sleepUploadOk;
extern String g_sleepUploadError;
extern String g_sleepUploadTmpPath;
extern File sleepUploadFile;

extern String lastPathComponent(const String& path);
extern String prettyRelativeLabel(const String& relPath);
extern String sanitizeFolderInput(const String& raw);
extern String sanitizeUploadedFilename(String fname);
extern bool ensureDirRecursive(const String& path);
extern bool isDirEmpty(const String& path);
extern void migrateBookMetadata(const String& oldPath, const String& newPath);
extern void resetUiEphemeralState();
extern void resetNavigationState();
extern void syncWakeState(bool reading);
extern void safeCloseBook();

// Helper escape functions
String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

void handleRoot() {
  // HTML stored in flash (PROGMEM) to avoid a ~50KB heap allocation that fails
  // when both WiFi and BLE stacks are active simultaneously
  static const char MAIN_HTML[] PROGMEM = R"rawhtml(
<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Pala One Dashboard</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700&display=swap" rel="stylesheet">
<style>
:root {
  --bg: #090d16;
  --card-bg: rgba(22, 30, 49, 0.7);
  --sidebar-bg: rgba(15, 23, 42, 0.6);
  --border: rgba(255, 255, 255, 0.08);
  --text: #f8fafc;
  --text-muted: #94a3b8;
  --primary: #6366f1;
  --primary-hover: #4f46e5;
  --success: #10b981;
  --danger: #ef4444;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: radial-gradient(circle at 50% 50%, #1e1b4b 0%, var(--bg) 100%);
  color: var(--text);
  font-family: 'Outfit', sans-serif;
  min-height: 100vh;
  display: flex;
}
.sidebar {
  width: 280px;
  background: var(--sidebar-bg);
  backdrop-filter: blur(16px);
  border-right: 1px solid var(--border);
  padding: 24px;
  display: flex;
  flex-direction: column;
  gap: 24px;
  flex-shrink: 0;
}
.logo-area {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.logo-title {
  font-size: 22px;
  font-weight: 700;
  background: linear-gradient(to right, #818cf8, #a78bfa);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}
.logo-subtitle {
  font-size: 12px;
  color: var(--text-muted);
}
.nav-menu {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.nav-btn {
  background: transparent;
  border: 1px solid transparent;
  color: var(--text-muted);
  padding: 12px 16px;
  border-radius: 10px;
  text-align: left;
  font-size: 15px;
  font-weight: 500;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 12px;
  transition: all 0.2s ease;
}
.nav-btn:hover {
  background: rgba(255,255,255,0.03);
  color: var(--text);
}
.nav-btn.active {
  background: var(--card-bg);
  border-color: var(--border);
  color: var(--text);
  box-shadow: 0 4px 20px rgba(0,0,0,0.15);
}
.folder-tree-title {
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--text-muted);
  margin-top: 16px;
}
.folder-tree {
  overflow-y: auto;
  flex-grow: 1;
}
.folder-node {
  list-style: none;
  margin-left: 12px;
}
.folder-link {
  color: var(--text-muted);
  text-decoration: none;
  font-size: 14px;
  padding: 6px 8px;
  display: flex;
  align-items: center;
  gap: 8px;
  border-radius: 6px;
  cursor: pointer;
}
.folder-link:hover, .folder-link.active {
  color: var(--text);
  background: rgba(255,255,255,0.02);
}
.main-content {
  flex-grow: 1;
  padding: 32px;
  overflow-y: auto;
  max-width: 1200px;
  margin: 0 auto;
  width: 100%;
}
.tab-content {
  display: none;
}
.tab-content.active {
  display: flex;
  flex-direction: column;
  gap: 24px;
}
.card {
  background: var(--card-bg);
  backdrop-filter: blur(12px);
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 24px;
  box-shadow: 0 8px 32px rgba(0,0,0,0.2);
}
h2 {
  font-size: 20px;
  font-weight: 600;
  margin-bottom: 16px;
}
p.muted {
  font-size: 14px;
  color: var(--text-muted);
  margin-bottom: 16px;
}
.explorer-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}
.breadcrumbs {
  font-size: 14px;
  color: var(--text-muted);
  display: flex;
  gap: 6px;
}
.breadcrumbs span {
  color: var(--text);
  font-weight: 500;
}
.breadcrumbs a {
  color: var(--primary);
  text-decoration: none;
}
.breadcrumbs a:hover {
  text-decoration: underline;
}
.explorer-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 16px;
}
.explorer-item {
  background: rgba(255,255,255,0.02);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 16px;
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 12px;
  cursor: pointer;
  position: relative;
  transition: all 0.2s ease;
}
.explorer-item:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--primary);
}
.explorer-icon {
  font-size: 40px;
}
.explorer-name {
  font-size: 14px;
  font-weight: 500;
  word-break: break-word;
  max-height: 40px;
  overflow: hidden;
}
.explorer-meta {
  font-size: 11px;
  color: var(--text-muted);
}
.progress-badge {
  position: absolute;
  top: 8px;
  right: 8px;
  background: var(--primary);
  font-size: 10px;
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 99px;
}
input[type=text], input[type=password], select {
  background: rgba(0,0,0,0.3);
  border: 1px solid var(--border);
  border-radius: 10px;
  color: var(--text);
  padding: 12px;
  width: 100%;
  font-family: inherit;
  font-size: 14px;
  transition: border-color 0.2s;
}
input[type=text]:focus, input[type=password]:focus, select:focus {
  border-color: var(--primary);
  outline: none;
}
.grid-2 {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}
.stack {
  display: flex;
  flex-direction: column;
  gap: 12px;
}
button {
  background: var(--primary);
  color: var(--text);
  font-weight: 600;
  border: 0;
  border-radius: 10px;
  padding: 12px 20px;
  font-family: inherit;
  font-size: 14px;
  cursor: pointer;
  transition: all 0.2s ease;
}
button:hover {
  background: var(--primary-hover);
  transform: translateY(-1px);
}
button.secondary {
  background: rgba(255,255,255,0.05);
  border: 1px solid var(--border);
}
button.secondary:hover {
  background: rgba(255,255,255,0.1);
}
button.danger {
  background: var(--danger);
}
button.danger:hover {
  background: #dc2626;
}
.drop-zone {
  border: 2px dashed rgba(255,255,255,0.2);
  border-radius: 16px;
  padding: 40px;
  text-align: center;
  cursor: pointer;
  background: rgba(255,255,255,0.01);
  transition: all 0.2s ease;
}
.drop-zone:hover, .drop-zone.dragover {
  border-color: var(--primary);
  background: rgba(99, 102, 241, 0.05);
}
.drawer {
  position: fixed;
  top: 0;
  right: -550px;
  width: 500px;
  height: 100vh;
  background: #111827;
  border-left: 1px solid var(--border);
  box-shadow: -10px 0 40px rgba(0,0,0,0.5);
  z-index: 100;
  display: flex;
  flex-direction: column;
  transition: right 0.3s cubic-bezier(0.16, 1, 0.3, 1);
}
.drawer.open {
  right: 0;
}
.drawer-header {
  padding: 20px;
  border-bottom: 1px solid var(--border);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.drawer-close {
  font-size: 24px;
  cursor: pointer;
  color: var(--text-muted);
}
.drawer-body {
  padding: 20px;
  flex-grow: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 20px;
}
.editor-textarea {
  width: 100%;
  flex-grow: 1;
  background: #030712;
  border: 1px solid var(--border);
  border-radius: 12px;
  color: var(--text);
  padding: 16px;
  font-family: monospace;
  font-size: 14px;
  line-height: 1.6;
  resize: none;
}
.editor-textarea:focus {
  border-color: var(--primary);
  outline: none;
}
.slider-container {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-top: 10px;
}
.slider-row {
  display: flex;
  align-items: center;
  gap: 12px;
}
input[type=range] {
  flex-grow: 1;
  accent-color: var(--primary);
}
.wifi-table {
  width: 100%;
  border-collapse: collapse;
  margin-top: 12px;
}
.wifi-table th, .wifi-table td {
  padding: 12px;
  text-align: left;
  border-bottom: 1px solid var(--border);
}
.wifi-table th {
  color: var(--text-muted);
  font-weight: 600;
  font-size: 13px;
}
.quick-links {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  margin-top: 12px;
}
.quick-link-card {
  background: rgba(255,255,255,0.02);
  border: 1px solid var(--border);
  padding: 12px 18px;
  border-radius: 10px;
  color: var(--text);
  text-decoration: none;
  font-size: 14px;
  font-weight: 500;
  display: flex;
  align-items: center;
  gap: 8px;
  transition: all 0.2s;
}
.quick-link-card:hover {
  background: rgba(255,255,255,0.05);
  border-color: var(--primary);
}
@media (max-width: 900px) {
  body { flex-direction: column; }
  .sidebar { width: 100%; height: auto; border-right: 0; border-bottom: 1px solid var(--border); }
  .drawer { width: 100%; right: -100%; }
}
</style>
</head>
<body>
<div class='sidebar'>
  <div class='logo-area'>
    <div class='logo-title'>Pala One</div>
    <div class='logo-subtitle'>Firmware v1.7.1</div>
  </div>
  <div class='nav-menu'>
    <button class='nav-btn active' onclick="switchTab('library')">&#128214; Library</button>
    <button class='nav-btn' onclick="switchTab('upload')">&#128229; Upload / Downloader</button>
    <button class='nav-btn' onclick="switchTab('settings')">&#9881; Settings</button>
    <button class='nav-btn' onclick="switchTab('dedrm')">&#128275; Calibre DeDRM</button>
  </div>
  <div class='folder-tree-title'>Folders</div>
  <div class='folder-tree' id='folder-tree-container'></div>
</div>
<div class='main-content'>
  <div id='tab-library' class='tab-content active'>
    <div class='card'>
      <div class='explorer-header'>
        <div class='breadcrumbs' id='library-breadcrumbs'></div>
        <button onclick='openCreateFolderModal()' class='secondary'>+ New Folder</button>
      </div>
      <div class='explorer-grid' id='library-grid'></div>
    </div>
  </div>

  <div id='tab-upload' class='tab-content'>
    <div class='card'>
      <h2>Upload Book</h2>
      <p class='muted'>Drag & drop files here or click to select from your device. EPUB files will be extracted and converted to plain text client-side instantly.</p>
      <div id='drop-zone' class='drop-zone'>
        <div class='drop-text'>Drag & drop files here or click to select</div>
        <input type='file' id='file-input' style='display:none' accept='.txt,.epub'>
      </div>
      <div id='progress-container' style='display:none; margin-top: 16px;'>
        <div class='muted' id='progress-text'>Uploading...</div>
        <div style='background:rgba(255,255,255,0.1); border-radius:99px; height:8px; overflow:hidden; margin-top:8px;'>
          <div id='progress-bar' style='background:var(--primary); height:100%; width:0%; transition:width 0.1s;'></div>
        </div>
      </div>
    </div>

    <div class='card'>
      <h2>Project Gutenberg Downloader</h2>
      <p class='muted'>Enter a Gutenberg Book ID or URL (e.g. 11 for Alice in Wonderland). The client proxy will download it with fallbacks and upload it instantly.</p>
      <div class='stack' style='flex-direction:row;'>
        <input type='text' id='gutenberg-input' placeholder='Gutenberg Book ID (e.g. 11)'>
        <button onclick='downloadGutenberg()'>Download</button>
      </div>
      <div id='gutenberg-status' class='muted' style='margin-top:10px; font-weight:500;'></div>
    </div>

    <div class='card'>
      <h2>Open Libraries</h2>
      <p class='muted'>Find open library sites to download free text-based books:</p>
      <div class='quick-links'>
        <a href='https://www.gutenberg.org/' target='_blank' class='quick-link-card'>&#127183; Project Gutenberg</a>
        <a href='https://standardebooks.org/' target='_blank' class='quick-link-card'>&#9889; Standard Ebooks</a>
        <a href='https://openlibrary.org/' target='_blank' class='quick-link-card'>&#128218; Open Library</a>
        <a href='https://manybooks.net/' target='_blank' class='quick-link-card'>&#128052; ManyBooks</a>
        <a href='https://librivox.org/' target='_blank' class='quick-link-card'>&#127911; LibriVox</a>
      </div>
    </div>

    <div class='card'>
      <h2>Bluetooth Upload (Web BLE)</h2>
      <p class='muted'>Upload files directly to Pala without home WiFi connection using Web Bluetooth.</p>
      <button id='ble-connect-btn' onclick='connectBLE()'>Connect via Bluetooth</button>
      <div id='ble-status-area' style='display:none; margin-top:16px;' class='stack'>
        <div class='muted'>Status: <span id='ble-status-pill' class='progress-badge' style='position:static;'>Connected</span></div>
        <input type='file' id='ble-file-input' accept='.txt,.epub' onchange='triggerBleUpload(this.files[0])'>
      </div>
    </div>
  </div>

  <div id='tab-settings' class='tab-content'>
    <div class='card'>
      <h2>Reading Preferences</h2>
      <form id='settings-form' onsubmit='saveSettings(event)' class='stack'>
        <div class='grid-2'>
          <div>
            <label>Font Size</label>
            <select name='font' id='setting-font'>
              <option value='8'>8px &mdash; tiny</option>
              <option value='10'>10px &mdash; small</option>
              <option value='12'>12px &mdash; medium</option>
              <option value='14'>14px &mdash; large</option>
            </select>
          </div>
          <div>
            <label>Auto-Sleep after</label>
            <select name='sleep' id='setting-sleep'>
              <option value='30'>30 seconds</option>
              <option value='60'>1 minute</option>
              <option value='120'>2 minutes</option>
              <option value='300'>5 minutes</option>
              <option value='600'>10 minutes</option>
              <option value='1800'>30 minutes</option>
            </select>
          </div>
        </div>
        <div class='grid-2'>
          <div>
            <label>Line Spacing</label>
            <select name='lgap' id='setting-lgap'>
              <option value='0'>0 px &mdash; compact</option>
              <option value='1'>1 px &mdash; normal</option>
              <option value='2'>2 px &mdash; relaxed</option>
              <option value='3'>3 px &mdash; loose</option>
            </select>
          </div>
          <div>
            <label style='display:flex; align-items:center; gap:8px; margin-top:32px;'>
              <input type='checkbox' name='cfg_invert' id='setting-invert'> Night Mode (Inverted Display)
            </label>
          </div>
        </div>
        <button type='submit'>Save Preferences</button>
      </form>
    </div>

    <div class='card'>
      <h2>Home WiFi Networks (Max 5)</h2>
      <p class='muted'>Configure your WiFi profiles. Pala connects to the strongest saved home network.</p>
      <table class='wifi-table'>
        <thead>
          <tr><th>SSID</th><th>Actions</th></tr>
        </thead>
        <tbody id='wifi-profiles-body'></tbody>
      </table>
      <form id='wifi-form' onsubmit='addWifi(event)' class='stack' style='margin-top:20px;'>
        <h3>Add WiFi Profile</h3>
        <div class='grid-2'>
          <input type='text' name='ssid' id='wifi-ssid-input' placeholder='WiFi SSID' required>
          <input type='password' name='pass' id='wifi-pass-input' placeholder='WiFi Password'>
        </div>
        <button type='submit'>Add Profile</button>
      </form>
    </div>

    <div class='card'>
      <h2>Spotify Integration</h2>
      <p class='muted'>Configure Spotify screensaver and currently-playing settings.</p>
      <form id='spotify-form' onsubmit='saveSpotify(event)' class='stack'>
        <label style='display:flex; align-items:center; gap:8px;'>
          <input type='checkbox' name='spot_scr' id='setting-spot-scr'> Enable Spotify Screensaver
        </label>
        <div>
          <label>Spotify Client ID</label>
          <input type='text' name='spot_id' id='setting-spot-id' placeholder='Your Spotify Client ID'>
        </div>
        <div>
          <label>Spotify Client Secret</label>
          <input type='password' name='spot_secret' id='setting-spot-secret' placeholder='Your Spotify Client Secret'>
        </div>
        <div>
          <label>Spotify Refresh Token</label>
          <input type='text' name='spot_refresh' id='setting-spot-refresh' placeholder='Your Spotify Refresh Token'>
        </div>
        <button type='submit'>Save Spotify API Config</button>
      </form>
    </div>

    <div class='card'>
      <h2>Chess Integration</h2>
      <p class='muted'>Configure your Chess Elo rating to fetch matching puzzles from Lichess.</p>
      <form id='chess-form' onsubmit='saveChess(event)' class='stack'>
        <div>
          <label>Chess Elo</label>
          <input type='text' name='chess_elo' id='setting-chess-elo' placeholder='Your Chess Elo (e.g. 1500)'>
        </div>
        <button type='submit'>Save Chess Config</button>
      </form>
    </div>

    <div class='card'>
      <h2>Screensaver Upload</h2>
      <p class='muted'>Upload custom screensaver image.</p>
      <form method='POST' action='/upload-sleep' enctype='multipart/form-data' class='stack'>
        <input type='file' name='file' accept='.bin' required>
        <button type='submit'>Upload Screensaver</button>
      </form>
    </div>

    <div class='card' style='border-color:rgba(239, 68, 68, 0.3);'>
      <h2>Device Reset</h2>
      <div class='grid-2'>
        <button onclick='location.reload()' class='secondary'>Refresh App</button>
        <button onclick='triggerFactoryReset()' class='danger'>Factory Reset Device</button>
      </div>
    </div>
  </div>

  <div id='tab-dedrm' class='tab-content'>
    <div class='card'>
      <h2>Calibre DeDRM Setup Guide</h2>
      <p class='muted'>Follow these steps on your PC/Mac to remove DRM from your purchased e-books:</p>
      <div class='stack' style='gap:16px;'>
        <div>
          <h3>Step 1: Install Calibre</h3>
          <p class='muted'>Install <a href='https://calibre-ebook.com/' target='_blank' style='color:var(--primary);'>Calibre</a>.</p>
        </div>
        <div>
          <h3>Step 2: Download DeDRM Plugin</h3>
          <p class='muted'>Download the latest DeDRM zip from Apprentice Harper / NoDRM GitHub page.</p>
        </div>
        <div>
          <h3>Step 3: Load Plugin into Calibre</h3>
          <p class='muted'>In Calibre: Preferences -> Plugins -> Load plugin from file. Select the downloaded zip.</p>
        </div>
        <div>
          <h3>Step 4: Configure Serial Keys</h3>
          <p class='muted'>Double-click DeDRM inside Calibre plugins list and enter Kindle Serial Number or Adobe ADE keys.</p>
        </div>
        <div>
          <h3>Step 5: Convert & Upload</h3>
          <p class='muted'>Import DRM books to Calibre. Convert them to clean TXT or EPUB, then upload here!</p>
        </div>
      </div>
    </div>
  </div>
</div>

<div class='drawer' id='editor-drawer'>
  <div class='drawer-header'>
    <h3 id='editor-title'>Edit Book</h3>
    <span class='drawer-close' onclick='closeEditor()'>&times;</span>
  </div>
  <div class='drawer-body'>
    <div class='slider-container'>
      <label>Reading Progress: <span id='editor-progress-val'>0%</span></label>
      <div class='slider-row'>
        <input type='range' id='editor-progress-slider' min='0' max='100' value='0' oninput='updateProgressLabel(this.value)'>
        <input type='number' id='editor-progress-num' min='0' max='100' value='0' style='width:60px; background:rgba(0,0,0,0.3); border:1px solid var(--border); color:#fff; border-radius:6px; padding:6px;' oninput='updateProgressSlider(this.value)'>
      </div>
      <button onclick='saveProgressRemote()'>Set Progress</button>
    </div>
    <div class='stack'>
      <label>Move to Folder:</label>
      <div class='slider-row'>
        <select id='editor-move-folder'></select>
        <button onclick='moveBookRemote()'>Move</button>
      </div>
    </div>
    <textarea class='editor-textarea' id='editor-text'></textarea>
    <div class='grid-2'>
      <button onclick='saveBookText()'>Save Content</button>
      <button onclick='deleteBookRemote()' class='danger'>Delete Book</button>
    </div>
  </div>
</div>

<script src='https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js'></script>
<script>
let currentFolder = '';
let booksData = { books: [], folders: [] };
let activeTab = 'library';
let activeBook = null;
let bleDevice = null;
let bleControlChar = null;
let bleDataChar = null;
let bleResponseResolver = null;

function switchTab(tabId) {
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.nav-btn').forEach(el => el.classList.remove('active'));
  document.getElementById('tab-' + tabId).classList.add('active');
  const btn = Array.from(document.querySelectorAll('.nav-btn')).find(b => b.getAttribute('onclick').includes(tabId));
  if (btn) btn.classList.add('active');
  activeTab = tabId;
}

async function loadData() {
  try {
    let res = await fetch('/api/books');
    booksData = await res.json();
    renderFolderTree();
    renderLibrary();
    populateMoveFolders();
  } catch (e) {
    console.error(e);
  }
  try {
    let res = await fetch('/api/wifi');
    let wifiData = await res.json();
    renderWifi(wifiData.profiles);
  } catch (e) {
    console.error(e);
  }
  try {
    let res = await fetch('/api/settings');
    let data = await res.json();
    document.getElementById('setting-font').value = data.fontSize;
    document.getElementById('setting-sleep').value = data.sleepSecs;
    document.getElementById('setting-lgap').value = data.lineGap;
    document.getElementById('setting-invert').checked = data.nightMode;
    document.getElementById('setting-spot-scr').checked = data.spotifyScreensaver;
    document.getElementById('setting-spot-id').value = data.spotifyClientId;
    document.getElementById('setting-spot-secret').value = data.spotifyClientSecret;
    document.getElementById('setting-spot-refresh').value = data.spotifyRefreshToken;
    if (data.chessElo !== undefined) {
      document.getElementById('setting-chess-elo').value = data.chessElo;
    }
  } catch (e) {
    console.error(e);
  }
}

function renderFolderTree() {
  const container = document.getElementById('folder-tree-container');
  container.innerHTML = '';
  let rootLi = document.createElement('div');
  rootLi.className = 'folder-link' + (currentFolder === '' ? ' active' : '');
  rootLi.innerHTML = '&#128194; [Root]';
  rootLi.onclick = () => selectFolder('');
  container.appendChild(rootLi);
  booksData.folders.forEach(f => {
    let li = document.createElement('div');
    li.className = 'folder-link' + (currentFolder === f ? ' active' : '');
    let depth = f.split('/').length - 1;
    li.style.marginLeft = (depth * 12 + 10) + 'px';
    li.innerHTML = '&#128193; ' + f.split('/').pop().replace(/_/g, ' ');
    li.onclick = () => selectFolder(f);
    container.appendChild(li);
  });
}

function selectFolder(folder) {
  currentFolder = folder;
  renderFolderTree();
  renderLibrary();
}

function renderLibrary() {
  const breadcrumbs = document.getElementById('library-breadcrumbs');
  breadcrumbs.innerHTML = '<a href="#" onclick="selectFolder(\'\')">Root</a>';
  if (currentFolder) {
    let parts = currentFolder.split('/');
    let accum = [];
    parts.forEach((p, idx) => {
      accum.push(p);
      let target = accum.join('/');
      breadcrumbs.innerHTML += ' / ';
      if (idx === parts.length - 1) {
        breadcrumbs.innerHTML += '<span>' + p.replace(/_/g, ' ') + '</span>';
      } else {
        breadcrumbs.innerHTML += `<a href="#" onclick="selectFolder('${target}')">${p.replace(/_/g, ' ')}</a>`;
      }
    });
  }

  const grid = document.getElementById('library-grid');
  grid.innerHTML = '';

  let subfolders = booksData.folders.filter(f => {
    if (!currentFolder) return !f.includes('/');
    let parts = f.split('/');
    return f.startsWith(currentFolder + '/') && parts.length === currentFolder.split('/').length + 1;
  });

  subfolders.forEach(f => {
    let card = document.createElement('div');
    card.className = 'explorer-item';
    card.innerHTML = `
      <div class='explorer-icon'>&#128193;</div>
      <div class='explorer-name'>${f.split('/').pop().replace(/_/g, ' ')}</div>
      <div class='explorer-meta'>Folder</div>
    `;
    card.onclick = () => selectFolder(f);
    grid.appendChild(card);
  });

  let files = booksData.books.filter(b => b.folder === currentFolder);
  files.forEach(b => {
    let card = document.createElement('div');
    card.className = 'explorer-item';
    let progressBadge = b.pct >= 0 ? `<div class='progress-badge'>${b.pct}%</div>` : '';
    card.innerHTML = `
      ${progressBadge}
      <div class='explorer-icon'>&#128213;</div>
      <div class='explorer-name'>${b.name}</div>
      <div class='explorer-meta'>${(b.size/1024).toFixed(1)} KB</div>
    `;
    card.onclick = () => openEditor(b);
    grid.appendChild(card);
  });
  if (subfolders.length === 0 && files.length === 0) {
    grid.innerHTML = "<div style='grid-column:1/-1; text-align:center; padding:40px; color:var(--text-muted);'>Folder is empty. Upload books to begin!</div>";
  }
}

function populateMoveFolders() {
  const select = document.getElementById('editor-move-folder');
  select.innerHTML = '<option value="">Root (/books)</option>';
  booksData.folders.forEach(f => {
    let opt = document.createElement('option');
    opt.value = f;
    opt.innerText = f;
    select.appendChild(opt);
  });
}

async function openEditor(book) {
  activeBook = book;
  document.getElementById('editor-title').innerText = book.name;
  document.getElementById('editor-progress-slider').value = book.pct >= 0 ? book.pct : 0;
  document.getElementById('editor-progress-num').value = book.pct >= 0 ? book.pct : 0;
  document.getElementById('editor-progress-val').innerText = (book.pct >= 0 ? book.pct : 0) + '%';
  document.getElementById('editor-move-folder').value = book.folder;
  const textEl = document.getElementById('editor-text');
  textEl.value = 'Loading book content...';
  document.getElementById('editor-drawer').classList.add('open');
  try {
    let res = await fetch('/download?path=' + encodeURIComponent(book.path));
    if (res.ok) textEl.value = await res.text();
    else textEl.value = 'Failed to load book text.';
  } catch (e) {
    textEl.value = 'Error fetching book content: ' + e;
  }
}

function closeEditor() {
  document.getElementById('editor-drawer').classList.remove('open');
  activeBook = null;
}

function updateProgressLabel(val) {
  document.getElementById('editor-progress-val').innerText = val + '%';
  document.getElementById('editor-progress-num').value = val;
}

function updateProgressSlider(val) {
  if (val < 0) val = 0;
  if (val > 100) val = 100;
  document.getElementById('editor-progress-val').innerText = val + '%';
  document.getElementById('editor-progress-slider').value = val;
}

async function saveProgressRemote() {
  if (!activeBook) return;
  let pct = document.getElementById('editor-progress-slider').value;
  try {
    let res = await fetch(`/set-page?path=${encodeURIComponent(activeBook.path)}&pct=${pct}`);
    if (res.ok) {
      alert('Progress updated!');
      loadData();
    } else {
      alert('Failed: ' + await res.text());
    }
  } catch(e) {
    alert(e);
  }
}

async function moveBookRemote() {
  if (!activeBook) return;
  let dest = document.getElementById('editor-move-folder').value;
  let formData = new FormData();
  formData.append('id', activeBook.id);
  formData.append('folder', dest);
  try {
    let res = await fetch('/move', { method: 'POST', body: formData });
    if (res.ok) {
      alert('Book moved!');
      closeEditor();
      loadData();
    } else {
      alert('Failed to move.');
    }
  } catch(e) {
    alert(e);
  }
}

async function saveBookText() {
  if (!activeBook) return;
  let text = document.getElementById('editor-text').value;
  try {
    let res = await fetch(`/save-book?path=${encodeURIComponent(activeBook.path)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: text
    });
    if (res.ok) {
      alert('Saved!');
      loadData();
    } else {
      alert('Failed.');
    }
  } catch (e) {
    alert(e);
  }
}

async function deleteBookRemote() {
  if (!activeBook) return;
  if (!confirm('Delete book?')) return;
  try {
    let res = await fetch('/del?id=' + activeBook.id);
    if (res.ok) {
      closeEditor();
      loadData();
    }
  } catch(e) {
    alert(e);
  }
}

async function openCreateFolderModal() {
  let name = prompt('New folder name:');
  if (!name) return;
  let formData = new FormData();
  let path = currentFolder ? (currentFolder + '/' + name) : name;
  formData.append('folder', path);
  try {
    let res = await fetch('/mkdir', { method: 'POST', body: formData });
    if (res.ok) loadData();
  } catch(e) {
    alert(e);
  }
}

function renderWifi(profiles) {
  const tbody = document.getElementById('wifi-profiles-body');
  tbody.innerHTML = '';
  profiles.forEach((p, idx) => {
    let tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${p.ssid}</td>
      <td><button class='danger' style='padding:6px 12px; font-size:12px;' onclick='deleteWifi(${idx})'>Delete</button></td>
    `;
    tbody.appendChild(tr);
  });
}

async function addWifi(e) {
  e.preventDefault();
  const form = document.getElementById('wifi-form');
  const formData = new FormData(form);
  try {
    let res = await fetch('/add-wifi', { method: 'POST', body: formData });
    if (res.ok) {
      form.reset();
      loadData();
    }
  } catch(e) {
    alert(e);
  }
}

async function deleteWifi(idx) {
  if (!confirm('Delete profile?')) return;
  try {
    let res = await fetch('/del-wifi?idx=' + idx);
    if (res.ok) loadData();
  } catch (e) {
    alert(e);
  }
}

async function saveSettings(e) {
  e.preventDefault();
  const form = document.getElementById('settings-form');
  const formData = new FormData(form);
  try {
    let res = await fetch('/settings', { method: 'POST', body: formData });
    if (res.ok) alert('Preferences saved!');
  } catch (e) {
    alert(e);
  }
}

async function saveSpotify(e) {
  e.preventDefault();
  const form = document.getElementById('spotify-form');
  const formData = new FormData(form);
  try {
    let res = await fetch('/settings', { method: 'POST', body: formData });
    if (res.ok) alert('Spotify saved!');
  } catch(e) {
    alert(e);
  }
}

async function saveChess(e) {
  e.preventDefault();
  const form = document.getElementById('chess-form');
  const formData = new FormData(form);
  try {
    let res = await fetch('/settings', { method: 'POST', body: formData });
    if (res.ok) alert('Chess Elo saved!');
  } catch (e) {
    alert(e);
  }
}

function triggerFactoryReset() {
  if (!confirm('Factory reset?')) return;
  fetch('/reset', { method: 'POST' }).then(() => {
    alert('Reset code sent! Reconnect to hotspot.');
    location.reload();
  });
}

function uploadFileAjax(file) {
  document.getElementById('progress-container').style.display = 'block';
  const pBar = document.getElementById('progress-bar');
  const pText = document.getElementById('progress-text');
  let xhr = new XMLHttpRequest();
  let formData = new FormData();
  formData.append('file', file);
  xhr.upload.addEventListener('progress', function(e) {
    if (e.lengthComputable) {
      let percent = Math.round((e.loaded / e.total) * 100);
      pBar.style.width = percent + '%';
      pText.innerText = 'Uploading: ' + percent + '%';
    }
  });
  xhr.addEventListener('load', function() {
    if (xhr.status === 200) {
      pText.innerText = 'Upload Successful!';
      setTimeout(() => {
        document.getElementById('progress-container').style.display = 'none';
        pBar.style.width = '0%';
        loadData();
      }, 1500);
    } else {
      pText.innerText = 'Upload Failed: ' + xhr.responseText;
      pBar.style.background = 'var(--danger)';
    }
  });
  xhr.open('POST', '/upload?folder=' + encodeURIComponent(currentFolder));
  xhr.send(formData);
}

async function convertEpubToTxt(file) {
  const zip = await JSZip.loadAsync(file);
  let htmlFiles = [];
  zip.forEach((path, entry) => {
    if (path.endsWith('.html') || path.endsWith('.xhtml')) htmlFiles.push(entry);
  });
  htmlFiles.sort((a, b) => a.name.localeCompare(b.name));
  let fullText = '';
  for (let entry of htmlFiles) {
    let content = await entry.async('text');
    let doc = new DOMParser().parseFromString(content, 'text/html');
    let text = doc.body ? doc.body.innerText : doc.documentElement.textContent;
    fullText += text + '\n\n';
  }
  if (fullText.trim().length === 0) throw new Error('No text inside EPUB.');
  let newName = file.name.replace(/\.epub$/i, '') + '.txt';
  return new File([fullText], newName, { type: 'text/plain' });
}

async function downloadGutenberg() {
  const idInput = document.getElementById('gutenberg-input').value.trim();
  if (!idInput) return;
  let match = idInput.match(/ebooks\/(\d+)/) || idInput.match(/files\/(\d+)/) || [null, idInput];
  let id = match[1];
  if (!/^\d+$/.test(id)) return;
  
  const statusEl = document.getElementById('gutenberg-status');
  statusEl.innerText = 'Downloading from Gutenberg...';
  
  const urls = [
    `https://www.gutenberg.org/ebooks/${id}.txt.utf-8`,
    `https://www.gutenberg.org/cache/epub/${id}/pg${id}.txt`,
    `https://www.gutenberg.org/files/${id}/${id}-0.txt`
  ];
  const proxies = [
    url => `https://api.allorigins.win/get?url=${encodeURIComponent(url)}`,
    url => `https://corsproxy.io/?${encodeURIComponent(url)}`
  ];
  let text = '';
  let filename = 'Gutenberg_' + id + '.txt';
  let success = false;
  for (let url of urls) {
    if (success) break;
    for (let getProxy of proxies) {
      try {
        let pUrl = getProxy(url);
        let res = await fetch(pUrl);
        if (!res.ok) continue;
        let contents = '';
        if (pUrl.includes('allorigins')) {
          let json = await res.json();
          contents = json.contents;
        } else {
          contents = await res.text();
        }
        if (contents && contents.trim().length > 1000) {
          text = contents;
          success = true;
          break;
        }
      } catch (e) {}
    }
  }
  if (!success) {
    statusEl.innerText = 'Failed to download.';
    return;
  }
  let titleMatch = text.match(/Title:\s*(.+)/i);
  let authorMatch = text.match(/Author:\s*(.+)/i);
  if (titleMatch) {
    let cleanTitle = titleMatch[1].replace(/[^a-zA-Z0-9]/g, '_').substring(0, 30);
    if (authorMatch) {
      let cleanAuthor = authorMatch[1].replace(/[^a-zA-Z0-9]/g, '_').substring(0, 15);
      filename = cleanTitle + '_by_' + cleanAuthor + '.txt';
    } else {
      filename = cleanTitle + '.txt';
    }
  }
  statusEl.innerText = 'Download complete! Uploading...';
  let file = new File([text], filename, { type: 'text/plain' });
  uploadFileAjax(file);
}

function handleBleNotification(event) {
  let value = new TextDecoder().decode(event.target.value);
  if (bleResponseResolver) bleResponseResolver(value);
}

async function connectBLE() {
  try {
    bleDevice = await navigator.bluetooth.requestDevice({
      filters: [{ name: 'Pala Reader' }],
      optionalServices: ['4fafc201-1fb5-459e-8fcc-c5c9c331914b']
    });
    const server = await bleDevice.gatt.connect();
    const service = await server.getPrimaryService('4fafc201-1fb5-459e-8fcc-c5c9c331914b');
    bleControlChar = await service.getCharacteristic('beb5483e-36e1-4688-b7f5-ea07361b26a8');
    bleDataChar = await service.getCharacteristic('cba1d00f-13d8-4f5b-9fca-dc5c9d1a3c7f');
    await bleControlChar.startNotifications();
    bleControlChar.addEventListener('characteristicvaluechanged', handleBleNotification);
    document.getElementById('ble-connect-btn').style.display = 'none';
    document.getElementById('ble-status-area').style.display = 'block';
  } catch (err) {
    alert(err.message);
  }
}

async function triggerBleUpload(file) {
  if (!file) return;
  let finalFile = file;
  if (file.name.toLowerCase().endsWith('.epub')) {
    try { finalFile = await convertEpubToTxt(file); } catch(e) { return; }
  }
  try {
    const arrayBuffer = await finalFile.arrayBuffer();
    const bytes = new Uint8Array(arrayBuffer);
    const totalSize = bytes.length;
    const startCmd = 'START:' + finalFile.name + ':' + totalSize;
    const startPromise = new Promise(resolve => { bleResponseResolver = resolve; });
    await bleControlChar.writeValue(new TextEncoder().encode(startCmd));
    const response = await startPromise;
    if (response !== 'OK') throw new Error('Rejected');
    const chunkSize = 244;
    let offset = 0;
    while (offset < totalSize) {
      const chunk = bytes.slice(offset, offset + chunkSize);
      await bleDataChar.writeValueWithoutResponse(chunk);
      offset += chunk.length;
      await new Promise(r => setTimeout(r, 12));
    }
    const endPromise = new Promise(resolve => { bleResponseResolver = resolve; });
    await bleControlChar.writeValue(new TextEncoder().encode('END'));
    const endResponse = await endPromise;
    if (endResponse === 'DONE') {
      alert('BLE Upload Complete!');
      loadData();
    }
  } catch (err) {
    alert(err.message);
  }
}

window.addEventListener('DOMContentLoaded', () => {
  loadData();
  const zone = document.getElementById('drop-zone');
  const input = document.getElementById('file-input');
  zone.addEventListener('click', () => input.click());
  zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('dragover'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('dragover'));
  zone.addEventListener('drop', async (e) => {
    e.preventDefault();
    zone.classList.remove('dragover');
    if (e.dataTransfer.files.length > 0) {
      let file = e.dataTransfer.files[0];
      if (file.name.toLowerCase().endsWith('.epub')) {
        try { file = await convertEpubToTxt(file); } catch (err) { return; }
      }
      uploadFileAjax(file);
    }
  });
  input.addEventListener('change', async () => {
    if (input.files.length > 0) {
      let file = input.files[0];
      if (file.name.toLowerCase().endsWith('.epub')) {
        try { file = await convertEpubToTxt(file); } catch (err) { return; }
      }
      uploadFileAjax(file);
    }
  });
});
</script>
</body>
</html>
)rawhtml";

  // Stream HTML from flash in 512-byte chunks to avoid large heap allocation
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  const size_t CHUNK = 512;
  size_t total = sizeof(MAIN_HTML) - 1;
  for (size_t i = 0; i < total; i += CHUNK) {
    size_t n = (total - i < CHUNK) ? (total - i) : CHUNK;
    server.sendContent_P(MAIN_HTML + i, n);
  }
  server.sendContent("");
}

void handleApiBooks() {
  loadBooks();
  String out = "{\"books\":[";
  for (int i = 0; i < bookCount; i++) {
    if (i > 0) out += ",";
    out += "{\"id\":" + String(i) + ",";
    out += "\"name\":\"" + jsonEscape(String(books[i].name)) + "\",";
    out += "\"path\":\"" + jsonEscape(String(books[i].path)) + "\",";
    out += "\"size\":" + String(books[i].size) + ",";
    out += "\"folder\":\"" + jsonEscape(String(books[i].folder)) + "\",";
    String key = prefKeyForBook(String(books[i].path));
    int pct = prefs.getInt((key + "_pct").c_str(), -1);
    out += "\"pct\":" + String(pct) + ",";
    int p = prefs.getInt((key + "_p").c_str(), 0);
    out += "\"page\":" + String(p);
    out += "}";
  }
  out += "],\"folders\":[";
  for (int i = 0; i < folderCount; i++) {
    if (i > 0) out += ",";
    out += "\"" + jsonEscape(String(folders[i])) + "\"";
  }
  out += "]}";
  server.send(200, "application/json; charset=utf-8", out);
}

void handleApiWifi() {
  loadWifiProfiles();
  String out = "{\"profiles\":[";
  for (int i = 0; i < g_wifiCount; i++) {
    if (i > 0) out += ",";
    out += "{\"ssid\":\"" + jsonEscape(g_wifiProfiles[i].ssid) + "\"}";
  }
  out += "]}";
  server.send(200, "application/json; charset=utf-8", out);
}

void handleApiSettings() {
  String out = "{";
  out += "\"fontSize\":" + String(g_fontSize) + ",";
  out += "\"sleepSecs\":" + String(g_sleepSecs) + ",";
  out += "\"lineGap\":" + String(LINE_GAP) + ",";
  out += "\"nightMode\":" + String(g_nightMode ? "true" : "false") + ",";
  out += "\"spotifyScreensaver\":" + String(g_spotifyScreensaver ? "true" : "false") + ",";
  out += "\"spotifyClientId\":\"" + jsonEscape(g_spotifyClientId) + "\",";
  out += "\"spotifyClientSecret\":\"" + jsonEscape(g_spotifyClientSecret) + "\",";
  out += "\"spotifyRefreshToken\":\"" + jsonEscape(g_spotifyRefreshToken) + "\",";
  out += "\"chessElo\":" + String(g_chessElo);
  out += "}";
  server.send(200, "application/json; charset=utf-8", out);
}

void handleAddWifi() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain; charset=utf-8", "missing ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if (g_wifiCount >= MAX_WIFI_PROFILES) {
    server.send(400, "text/plain; charset=utf-8", "WiFi profile limit reached");
    return;
  }
  g_wifiProfiles[g_wifiCount].ssid = ssid;
  g_wifiProfiles[g_wifiCount].pass = pass;
  g_wifiCount++;
  saveWifiProfiles();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleDelWifi() {
  if (!server.hasArg("idx")) {
    server.send(400, "text/plain; charset=utf-8", "missing idx");
    return;
  }
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= g_wifiCount) {
    server.send(400, "text/plain; charset=utf-8", "bad idx");
    return;
  }
  for (int i = idx; i < g_wifiCount - 1; i++) {
    g_wifiProfiles[i] = g_wifiProfiles[i + 1];
  }
  g_wifiCount--;
  saveWifiProfiles();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleDownload() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain; charset=utf-8", "missing path");
    return;
  }
  String path = server.arg("path");
  if (!FS.exists(path)) {
    server.send(404, "text/plain; charset=utf-8", "file not found");
    return;
  }
  File f = FS.open(path, "r");
  if (!f) {
    server.send(500, "text/plain; charset=utf-8", "open failed");
    return;
  }
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

void handleSaveBook() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain; charset=utf-8", "missing path");
    return;
  }
  String path = server.arg("path");
  String text = server.arg("plain");
  File f = FS.open(path, "w");
  if (!f) {
    server.send(500, "text/plain; charset=utf-8", "open failed");
    return;
  }
  f.print(normalizeTypography(text));
  f.close();
  if (currentBookPath == path) {
    clearCurrentBookState();
    reopenCurrentBookIfNeeded();
  }
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleSetPage() {
  if (!server.hasArg("path") || (!server.hasArg("page") && !server.hasArg("pct"))) {
    server.send(400, "text/plain; charset=utf-8", "missing parameters");
    return;
  }
  String path = server.arg("path");
  String key = prefKeyForBook(path);
  int targetPage = -1;
  if (server.hasArg("page")) {
    targetPage = server.arg("page").toInt();
  } else if (server.hasArg("pct")) {
    int pct = server.arg("pct").toInt();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    File f = FS.open(path, "r");
    if (f) {
      size_t total = f.size();
      size_t targetOff = (total * pct) / 100;
      int p = 0;
      uint32_t off = 0;
      while (off < targetOff) {
        uint32_t next = buildNextOffsetFor(f, off);
        if (next == off || next >= total) break;
        off = next;
        p++;
      }
      targetPage = p;
      f.close();
    }
  }
  if (targetPage >= 0) {
    prefs.putInt((key + "_p").c_str(), targetPage);
    File f = FS.open(path, "r");
    if (f) {
      size_t total = f.size();
      uint32_t off = pageOffsetForPage(f, targetPage);
      int pct = (total > 0) ? (int)((off * 100) / total) : 0;
      prefs.putInt((key + "_pct").c_str(), pct);
      f.close();
    }
    if (currentBookPath == path) {
      pageIndex = targetPage;
      if (mode == MODE_READER) renderCurrentPage();
    }
    server.send(200, "text/plain; charset=utf-8", "OK");
  } else {
    server.send(500, "text/plain; charset=utf-8", "failed to set page");
  }
}

void handleDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain; charset=utf-8", "missing id");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= bookCount) {
    server.send(400, "text/plain; charset=utf-8", "bad id");
    return;
  }
  String path = String(books[id].path);
  if (currentBookPath == path) {
    clearCurrentBookState();
    resetPreviewState();
    syncWakeState(false);
  }
  if (FS.exists(path)) FS.remove(path);
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleCreateFolder() {
  ensureBooksDir();
  if (!server.hasArg("folder")) {
    server.send(400, "text/plain; charset=utf-8", "missing folder");
    return;
  }
  String folder = sanitizeFolderInput(server.arg("folder"));
  if (folder.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "bad folder");
    return;
  }
  if (folderCount >= MAX_FOLDERS) {
    server.send(409, "text/plain; charset=utf-8", "folder limit reached");
    return;
  }
  String fullPath = "/books/" + folder;
  if (!ensureDirRecursive(fullPath)) {
    server.send(500, "text/plain; charset=utf-8", "mkdir failed");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleDeleteFolder() {
  if (!server.hasArg("folder")) {
    server.send(400, "text/plain; charset=utf-8", "missing folder");
    return;
  }
  String folder = sanitizeFolderInput(server.arg("folder"));
  if (folder.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "bad folder");
    return;
  }
  String fullPath = "/books/" + folder;
  if (!FS.exists(fullPath)) {
    server.send(404, "text/plain; charset=utf-8", "folder not found");
    return;
  }
  if (!isDirEmpty(fullPath)) {
    server.send(409, "text/plain; charset=utf-8", "folder not empty");
    return;
  }
  if (!FS.rmdir(fullPath)) {
    server.send(500, "text/plain; charset=utf-8", "delete failed");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleMoveBook() {
  loadBooks();
  if (!server.hasArg("id")) {
    server.send(400, "text/plain; charset=utf-8", "missing id");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= bookCount) {
    server.send(400, "text/plain; charset=utf-8", "bad id");
    return;
  }
  String oldPath = String(books[id].path);
  String folder = sanitizeFolderInput(server.arg("folder"));
  String destDir = (folder.length() == 0) ? String("/books") : String("/books/") + folder;
  if (!ensureDirRecursive(destDir)) {
    server.send(500, "text/plain; charset=utf-8", "folder create failed");
    return;
  }
  String newPath = destDir + "/" + lastPathComponent(oldPath);
  if (newPath == oldPath) {
    server.send(200, "text/plain; charset=utf-8", "OK");
    return;
  }
  if (FS.exists(newPath)) {
    server.send(409, "text/plain; charset=utf-8", "destination exists");
    return;
  }
  bool wasCurrent = (currentBookPath == oldPath);
  if (wasCurrent && bookFile) bookFile.close();
  if (!FS.rename(oldPath, newPath)) {
    server.send(500, "text/plain; charset=utf-8", "move failed");
    return;
  }
  migrateBookMetadata(oldPath, newPath);
  if (wasCurrent) {
    bookFile = FS.open(newPath, "r");
  }
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleUploadDone() {
  if (g_uploadOk) {
    loadBooks();
    server.send(200, "text/plain; charset=utf-8", "OK");
  } else {
    server.send(400, "text/plain; charset=utf-8", g_uploadError.length() ? g_uploadError : "Upload failed");
  }
}

void handleResetConfirm() {
  server.send(200, "text/plain", "OK");
}

void doFactoryReset() {
  safeCloseBook();
  clearCurrentBookState();
  resetUiEphemeralState();
  resetNavigationState();
  syncWakeState(false);
  prefs.clear();
  FS.end();
  delay(100);
  FS.format();
  delay(200);
  if (!FS.begin(true)) return;
  ensureBooksDir();
  loadBooks();
}

void handleResetDo() {
  doFactoryReset();
  server.send(200, "text/plain; charset=utf-8", "Reset done.");
}

void handleSettingsPost() {
  if (server.hasArg("font")) {
    int fs = server.arg("font").toInt();
    if (fs != 8 && fs != 10 && fs != 12 && fs != 14) fs = 10;
    applyFontSize(fs);
    prefs.putInt("cfg_font", fs);
  }
  if (server.hasArg("sleep")) {
    int ss = server.arg("sleep").toInt();
    if (ss < 10) ss = 10;
    if (ss > 3600) ss = 3600;
    g_sleepSecs = (uint32_t)ss;
    SLEEP_AFTER_MS = g_sleepSecs * 1000UL;
    prefs.putInt("cfg_sleep", ss);
  }
  if (server.hasArg("lgap")) {
    int lg = server.arg("lgap").toInt();
    if (lg < 0) lg = 0;
    if (lg > 4) lg = 4;
    LINE_GAP = lg;
    invalidateMetrics();
    prefs.putInt("cfg_lgap", lg);
  }
  if (server.hasArg("cfg_invert")) {
    g_nightMode = (server.arg("cfg_invert") == "true" || server.arg("cfg_invert") == "1" || server.arg("cfg_invert") == "on");
  } else if (server.hasArg("font") || server.hasArg("sleep") || server.hasArg("lgap")) {
    g_nightMode = false;
  }
  prefs.putBool("cfg_invert", g_nightMode);

  if (server.hasArg("spot_id")) {
    g_spotifyClientId = server.arg("spot_id");
    prefs.putString("spot_id", g_spotifyClientId);
  }
  if (server.hasArg("spot_secret")) {
    g_spotifyClientSecret = server.arg("spot_secret");
    prefs.putString("spot_secret", g_spotifyClientSecret);
  }
  if (server.hasArg("spot_refresh")) {
    g_spotifyRefreshToken = server.arg("spot_refresh");
    prefs.putString("spot_refresh", g_spotifyRefreshToken);
  }
  if (server.hasArg("spot_scr")) {
    g_spotifyScreensaver = (server.arg("spot_scr") == "true" || server.arg("spot_scr") == "1" || server.arg("spot_scr") == "on");
  } else if (server.hasArg("spot_id") || server.hasArg("spot_secret") || server.hasArg("spot_refresh")) {
    g_spotifyScreensaver = false;
  }
  prefs.putBool("spot_scr", g_spotifyScreensaver);

  if (server.hasArg("chess_elo")) {
    g_chessElo = server.arg("chess_elo").toInt();
    if (g_chessElo < 100) g_chessElo = 100;
    if (g_chessElo > 4000) g_chessElo = 4000;
    prefs.putInt("cfg_chess_elo", g_chessElo);
  }

  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleDeleteSleepImg() {
  if (FS.exists("/sleep.bin")) FS.remove("/sleep.bin");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleUploadSleepDone() {
  if (g_sleepUploadOk) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(400, "text/plain; charset=utf-8", g_sleepUploadError.length() ? g_sleepUploadError : "Sleep image upload failed");
  }
}

#endif // WEB_SPA_H
