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
extern int g_screensaverMode;
extern String g_calUrl;
extern int g_timezoneOffsetHours;

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
extern size_t getFolderSize(const String& path);

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
    <div class='logo-subtitle'>Firmware v1.7.3</div>
  </div>
  <div class='nav-menu'>
    <button class='nav-btn active' onclick="switchTab('library')">&#128214; Home</button>
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
        <input type='file' id='file-input' style='display:none' accept='.txt,.epub,.pdf'>
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
        <input type='file' id='ble-file-input' accept='.txt,.epub,.pdf' onchange='triggerBleUpload(this.files[0])'>
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
            <label>Screensaver Mode</label>
            <select name='scr_mode' id='setting-scr-mode'>
              <option value='0'>Picture</option>
              <option value='1'>Spotify</option>
              <option value='2'>Chess Daily Board</option>
              <option value='3'>Minimalist Clock</option>
            </select>
          </div>
        </div>
        <div class='grid-2'>
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
      <h2>Storage Status</h2>
      <p class='muted'>LittleFS Partition Information.</p>
      <div class='stack' style='gap:12px;'>
        <div style='display:flex; justify-content:space-between; font-size:14px; font-weight:500;'>
          <span>Used Space: <span id='storage-used-txt'>0 KB</span> (<span id='storage-pct-txt'>0%</span>)</span>
          <span>Total Size: <span id='storage-total-txt'>0 KB</span></span>
        </div>
        <div style='background:rgba(255,255,255,0.08); border-radius:99px; height:12px; overflow:hidden;'>
          <div id='storage-bar' style='background:linear-gradient(to right, #6366f1, #a78bfa); height:100%; width:0%; transition:width 0.3s;'></div>
        </div>
        <div style='display:flex; gap:16px; font-size:13px; color:var(--text-muted);'>
          <span>&#128214; Books: <span id='storage-books-txt'>0 KB</span></span>
          <span>&#128190; Free: <span id='storage-free-txt'>0 KB</span></span>
        </div>
      </div>
    </div>

    <div class='card'>
      <h2>Calendar Integration</h2>
      <p class='muted'>Configure your Google Calendar URL (ICS or plain text) and timezone offset.</p>
      <form id='calendar-form' onsubmit='saveCalendar(event)' class='stack'>
        <div>
          <label>Calendar URLs (One per line, or comma-separated)</label>
          <textarea name='cal_url' id='setting-cal-url' placeholder='https://.../basic.ics&#10;https://.../other.ics' style='width:100%; height:80px; background:rgba(0,0,0,0.3); border:1px solid var(--border); border-radius:10px; color:var(--text); padding:12px; font-family:inherit; font-size:14px; resize:vertical;'></textarea>
        </div>
        <div class='grid-2'>
          <div>
            <label>Timezone Offset (Hours from UTC)</label>
            <select name='tz_offset' id='setting-tz-offset'>
              <option value='-12'>UTC -12</option>
              <option value='-11'>UTC -11</option>
              <option value='-10'>UTC -10</option>
              <option value='-9'>UTC -9</option>
              <option value='-8'>UTC -8</option>
              <option value='-7'>UTC -7</option>
              <option value='-6'>UTC -6</option>
              <option value='-5'>UTC -5</option>
              <option value='-4'>UTC -4</option>
              <option value='-3'>UTC -3</option>
              <option value='-2'>UTC -2</option>
              <option value='-1'>UTC -1</option>
              <option value='0'>UTC +0</option>
              <option value='1'>UTC +1</option>
              <option value='2'>UTC +2 (Europe/Berlin)</option>
              <option value='3'>UTC +3</option>
              <option value='4'>UTC +4</option>
              <option value='5'>UTC +5</option>
              <option value='6'>UTC +6</option>
              <option value='7'>UTC +7</option>
              <option value='8'>UTC +8</option>
              <option value='9'>UTC +9</option>
              <option value='10'>UTC +10</option>
              <option value='11'>UTC +11</option>
              <option value='12'>UTC +12</option>
            </select>
          </div>
        </div>
        <button type='submit'>Save Calendar Config</button>
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
      <h2>Interactive Screensaver Creator</h2>
      <p class='muted'>Upload any JPEG/PNG image, customize it with positioning/zoom and custom text overlay, dither/process it for the e-ink screen, and save or upload directly to your Pala.</p>
      
      <div style="display: flex; flex-direction: column; align-items: center; gap: 16px; margin-bottom: 24px;">
        <div style="background: #27272a; border-radius: 12px; padding: 12px; border: 1px solid var(--border); box-shadow: 0 8px 24px rgba(0,0,0,0.4); max-width: 100%;">
          <canvas id="screensaver-canvas" width="250" height="122" style="background: #fff; border: 4px solid #111; display: block; image-rendering: pixelated; width: 250px; height: 122px; cursor: move;"></canvas>
          <div style="text-align: center; font-size: 11px; color: var(--text-muted); margin-top: 6px;">Live Preview (250 x 122) — Drag image or text on canvas to reposition</div>
        </div>
      </div>

      <div class="stack" style="gap: 16px;">
        <div class="grid-2">
          <div>
            <label>1. Select Image</label>
            <input type="file" id="scr-image-input" accept="image/*">
          </div>
          <div>
            <label>Drag Target Mode</label>
            <select id="scr-drag-mode">
              <option value="image">Move Image</option>
              <option value="text">Move Text Overlay</option>
            </select>
          </div>
        </div>

        <div class="grid-2">
          <div>
            <label>Image Zoom (Scale: <span id="scr-scale-val">1.0</span>x)</label>
            <input type="range" id="scr-image-scale" min="0.1" max="4.0" step="0.02" value="1.0" oninput="updateScrScale(this.value)">
          </div>
          <div style="display:flex; align-items:center; gap:20px; margin-top: 24px;">
            <label style="display:flex; align-items:center; gap:8px;">
              <input type="checkbox" id="scr-dither-enable" checked onchange="drawScreensaverPreview()"> Floyd-Steinberg Dithering
            </label>
            <label style="display:flex; align-items:center; gap:8px;">
              <input type="checkbox" id="scr-invert-enable" onchange="drawScreensaverPreview()"> Invert Colors
            </label>
          </div>
        </div>

        <div class="grid-2">
          <div>
            <label>Binarization Threshold (<span id="scr-threshold-val">127</span>)</label>
            <input type="range" id="scr-image-threshold" min="0" max="255" step="1" value="127" oninput="updateScrThreshold(this.value)">
          </div>
          <div>
            <label>Image Offset Position</label>
            <div style="display:flex; gap: 8px;">
              <input type="number" id="scr-image-x" value="0" style="width:70px;" placeholder="X" oninput="updateScrImagePosInputs()">
              <input type="number" id="scr-image-y" value="0" style="width:70px;" placeholder="Y" oninput="updateScrImagePosInputs()">
              <button onclick="resetScrImagePos()" class="secondary" style="padding: 10px 14px;">Reset Pos</button>
            </div>
          </div>
        </div>

        <hr style="border: 0; border-top: 1px solid var(--border); margin: 8px 0;">

        <div class="grid-2">
          <div>
            <label>2. Add Text Overlay</label>
            <input type="text" id="scr-text-input" placeholder="Type overlay text here..." oninput="drawScreensaverPreview()">
          </div>
          <div>
            <label>Text Position & Styling</label>
            <div style="display:flex; gap: 8px; align-items: center;">
              <input type="number" id="scr-text-x" value="125" style="width:70px;" placeholder="X" oninput="updateScrTextPosInputs()">
              <input type="number" id="scr-text-y" value="110" style="width:70px;" placeholder="Y" oninput="updateScrTextPosInputs()">
              <select id="scr-text-color" style="width: 90px; padding: 10px;" onchange="updateScrTextColor(this.value)">
                <option value="black">Black</option>
                <option value="white" selected>White</option>
              </select>
            </div>
          </div>
        </div>

        <div class="grid-2">
          <div>
            <label>Text Font Size (<span id="scr-text-size-val">12</span>px)</label>
            <input type="range" id="scr-text-size" min="8" max="32" step="1" value="12" oninput="updateScrTextSize(this.value)">
          </div>
          <div>
            <label>Text Font Family</label>
            <select id="scr-text-font" onchange="updateScrTextFont(this.value)">
              <option value="sans-serif">Sans-Serif (Outfit / Arial)</option>
              <option value="serif">Serif (Times / Georgia)</option>
              <option value="monospace">Monospace (Courier)</option>
              <option value="Impact">Impact (Bold Heading)</option>
            </select>
          </div>
        </div>

        <div class="grid-2" style="margin-top: 12px;">
          <button onclick="uploadScreensaverBin()">Upload Screensaver to Pala</button>
          <button onclick="downloadScreensaverBin()" class="secondary">Download .bin File</button>
        </div>
      </div>
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
    if (data.screensaverMode !== undefined) {
      document.getElementById('setting-scr-mode').value = data.screensaverMode;
    }
    if (data.calendarUrl !== undefined) {
      document.getElementById('setting-cal-url').value = data.calendarUrl.replace(/,/g, '\n');
    }
    if (data.timezoneOffset !== undefined) {
      document.getElementById('setting-tz-offset').value = data.timezoneOffset;
    }
    if (data.storageTotal !== undefined) {
      document.getElementById('storage-total-txt').innerText = (data.storageTotal / 1024).toFixed(1) + ' KB';
      document.getElementById('storage-used-txt').innerText = (data.storageUsed / 1024).toFixed(1) + ' KB';
      document.getElementById('storage-books-txt').innerText = (data.storageBooks / 1024).toFixed(1) + ' KB';
      document.getElementById('storage-free-txt').innerText = (data.storageFree / 1024).toFixed(1) + ' KB';
      document.getElementById('storage-pct-txt').innerText = data.storagePct + '%';
      document.getElementById('storage-bar').style.width = data.storagePct + '%';
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
  select.innerHTML = '<option value="">Root (/reading)</option>';
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

async function saveCalendar(e) {
  e.preventDefault();
  const form = document.getElementById('calendar-form');
  const formData = new FormData(form);
  let calUrl = formData.get('cal_url') || '';
  calUrl = calUrl.replace(/\r?\n/g, ',');
  formData.set('cal_url', calUrl);
  try {
    let res = await fetch('/settings', { method: 'POST', body: formData });
    if (res.ok) alert('Calendar settings saved!');
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
  xhr.open('POST', '/upload');
  xhr.send(formData);
}

function checkDragAndDrop() {
  const zone = document.getElementById('drop-zone');
  const input = document.getElementById('file-input');
  zone.onclick = () => input.click();
  input.onchange = () => { if (input.files.length) processSelectedFile(input.files[0]); };
  zone.ondragover = zone.ondragenter = (e) => { e.preventDefault(); zone.classList.add('dragover'); };
  zone.ondragleave = zone.ondragend = () => zone.classList.remove('dragover');
  zone.ondrop = (e) => { e.preventDefault(); zone.classList.remove('dragover'); if (e.dataTransfer.files.length) processSelectedFile(e.dataTransfer.files[0]); };
}

async function processSelectedFile(file) {
  if (file.name.endsWith('.epub')) {
    document.getElementById('progress-container').style.display = 'block';
    const pText = document.getElementById('progress-text');
    pText.innerText = 'Parsing EPUB Client-Side...';
    try {
      let jsZip = new JSZip();
      let zip = await jsZip.loadAsync(file);
      let opfPath = '';
      for (let filename in zip.files) {
        if (filename.endsWith('.opf')) { opfPath = filename; break; }
      }
      if (!opfPath) throw new Error('Invalid EPUB (no opf file)');
      let opfText = await zip.file(opfPath).async('text');
      let parser = new DOMParser();
      let xml = parser.parseFromString(opfText, 'text/xml');
      let manifest = {};
      let itemEls = xml.getElementsByTagName('item');
      for (let i = 0; i < itemEls.length; i++) {
        let id = itemEls[i].getAttribute('id');
        let href = itemEls[i].getAttribute('href');
        manifest[id] = href;
      }
      let itemrefs = xml.getElementsByTagName('itemref');
      let spine = [];
      for (let i = 0; i < itemrefs.length; i++) {
        spine.push(itemrefs[i].getAttribute('idref'));
      }
      let opfDir = opfPath.substring(0, opfPath.lastIndexOf('/') + 1);
      let plainText = '';
      for (let idref of spine) {
        let rel = manifest[idref];
        if (rel) {
          let fullRel = opfDir + rel;
          let entry = zip.file(fullRel);
          if (!entry) {
            let cleanRel = rel.split('#')[0];
            entry = zip.file(opfDir + cleanRel);
          }
          if (entry) {
            let html = await entry.async('text');
            let doc = parser.parseFromString(html, 'text/html');
            let text = doc.body ? doc.body.innerText : doc.documentElement.innerText;
            plainText += text + '\n\n';
          }
        }
      }
      if (plainText.trim().length === 0) throw new Error('EPUB conversion produced empty text');
      let txtName = file.name.substring(0, file.name.lastIndexOf('.')) + '.txt';
      let convertedFile = new File([plainText], txtName, { type: 'text/plain' });
      uploadFileAjax(convertedFile);
    } catch (e) {
      alert('EPUB Conversion Failed: ' + e.message);
      document.getElementById('progress-container').style.display = 'none';
    }
  } else if (file.name.endsWith('.pdf')) {
    document.getElementById('progress-container').style.display = 'block';
    const pText = document.getElementById('progress-text');
    pText.innerText = 'Converting PDF Client-Side...';
    try {
      if (!window.pdfjsLib) {
        pText.innerText = 'Loading PDF.js Library...';
        const script = document.createElement('script');
        script.src = 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/2.16.105/pdf.min.js';
        document.head.appendChild(script);
        await new Promise(r => script.onload = r);
        pdfjsLib.GlobalWorkerOptions.workerSrc = 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/2.16.105/pdf.worker.min.js';
      }
      pText.innerText = 'Extracting Text from PDF...';
      const arrayBuffer = await file.arrayBuffer();
      const pdf = await pdfjsLib.getDocument({data: arrayBuffer}).promise;
      let text = '';
      for (let i = 1; i <= pdf.numPages; i++) {
        const page = await pdf.getPage(i);
        const content = await page.getTextContent();
        text += content.items.map(item => item.str).join(' ') + '\n\n';
      }
      let txtName = file.name.replace(/\.pdf$/i, '.txt');
      let newFile = new File([text], txtName, {type: 'text/plain'});
      uploadFileAjax(newFile);
    } catch (e) {
      alert('Error parsing PDF: ' + e.message);
      document.getElementById('progress-container').style.display = 'none';
    }
  } else {
    uploadFileAjax(file);
  }
}

async function downloadGutenberg() {
  const input = document.getElementById('gutenberg-input').value.trim();
  const status = document.getElementById('gutenberg-status');
  if (!input) return;
  status.innerText = 'Initiating download request...';
  let id = input;
  if (input.includes('gutenberg.org/')) {
    let match = input.match(/\/ebooks\/(\d+)/) || input.match(/\/files\/(\d+)/);
    if (match) id = match[1];
  }
  let urls = [
    `https://www.gutenberg.org/files/${id}/${id}-0.txt`,
    `https://www.gutenberg.org/cache/epub/${id}/pg${id}.txt`,
    `https://raw.githubusercontent.com/wgroeneveld/gutenberg-mirror/master/txt/${id}/${id}.txt`
  ];
  let success = false;
  for (let url of urls) {
    status.innerText = 'Trying download from mirror: ' + new URL(url).hostname;
    try {
      let res = await fetch('https://api.allorigins.win/get?url=' + encodeURIComponent(url));
      if (!res.ok) continue;
      let wrapper = await res.json();
      let contents = wrapper.contents;
      if (contents && contents.length > 500) {
        let txtFile = new File([contents], `gutenberg_${id}.txt`, { type: 'text/plain' });
        uploadFileAjax(txtFile);
        status.innerText = 'Download success! Transferring to e-reader...';
        success = true;
        break;
      }
    } catch (e) {}
  }
  if (!success) {
    status.innerText = 'Failed to download from standard mirrors. Please upload manually.';
  }
}

function initBLEService() {
  // Web Bluetooth service implementation
}

async function connectBLE() {
  const btn = document.getElementById('ble-connect-btn');
  try {
    bleDevice = await navigator.bluetooth.requestDevice({
      filters: [{ name: 'Pala Reader' }],
      optionalServices: ['4fafc201-1fb5-459e-8fcc-c5c9c331914b']
    });
    let server = await bleDevice.gatt.connect();
    let service = await server.getPrimaryService('4fafc201-1fb5-459e-8fcc-c5c9c331914b');
    bleControlChar = await service.getCharacteristic('beb5483e-36e1-4688-b7f5-ea07361b26a8');
    bleDataChar = await service.getCharacteristic('cba1d00f-13d8-4f5b-9fca-dc5c9d1a3c7f');
    bleDevice.addEventListener('gattserverdisconnected', onBleDisconnected);
    document.getElementById('ble-status-area').style.display = 'block';
    btn.style.display = 'none';
    await bleControlChar.startNotifications();
    bleControlChar.addEventListener('characteristicvaluechanged', handleBleNotification);
  } catch (e) {
    alert('BLE connection failed: ' + e);
  }
}

function onBleDisconnected() {
  document.getElementById('ble-status-area').style.display = 'none';
  document.getElementById('ble-connect-btn').style.display = 'inline-block';
  bleDevice = null;
  bleControlChar = null;
  bleDataChar = null;
}

function handleBleNotification(e) {
  let val = new TextDecoder().decode(e.target.value);
  if (bleResponseResolver) {
    bleResponseResolver(val);
    bleResponseResolver = null;
  }
}

function sendBleControl(cmd) {
  return new Promise(async (resolve) => {
    bleResponseResolver = resolve;
    await bleControlChar.writeValue(new TextEncoder().encode(cmd));
  });
}

async function triggerBleUpload(file) {
  if (!file) return;
  if (file.name.endsWith('.epub')) {
    alert('BLE currently only supports .txt files directly. Convert to .txt on PC or use WiFi dashboard.');
    return;
  }
  const statusPill = document.getElementById('ble-status-pill');
  statusPill.innerText = 'Starting...';
  try {
    let reader = new FileReader();
    reader.onload = async function() {
      let arrayBuffer = reader.result;
      let bytes = new Uint8Array(arrayBuffer);
      let response = await sendBleControl(`START:${file.name}:${bytes.length}`);
      if (response !== 'OK') {
        alert('Upload failed: ' + response);
        statusPill.innerText = 'Failed';
        return;
      }
      let chunkSize = 512;
      let offset = 0;
      while (offset < bytes.length) {
        let chunk = bytes.slice(offset, offset + chunkSize);
        await bleDataChar.writeValueWithoutResponse(chunk);
        offset += chunkSize;
        let progress = Math.round((offset / bytes.length) * 100);
        statusPill.innerText = `Uploading: ${progress > 100 ? 100 : progress}%`;
        await new Promise(r => setTimeout(r, 15));
      }
      let endRes = await sendBleControl('END');
      if (endRes === 'DONE') {
        statusPill.innerText = 'Success!';
        loadData();
      } else {
        alert('Finalize failed: ' + endRes);
        statusPill.innerText = 'Failed';
      }
    };
    reader.readAsArrayBuffer(file);
  } catch (e) {
    alert('BLE Upload failed: ' + e);
    statusPill.innerText = 'Failed';
  }
}



// Interactive Screensaver Designer variables
let scrImage = null;
let scrScale = 1.0;
let scrImgX = 0;
let scrImgY = 0;
let scrTextX = 125;
let scrTextY = 110;
let scrTextSize = 12;
let scrTextColor = "white";
let scrTextFont = "sans-serif";
let scrIsDragging = false;
let scrDragStartX = 0;
let scrDragStartY = 0;

function initScreensaverDesigner() {
  const canvas = document.getElementById('screensaver-canvas');
  const imageInput = document.getElementById('scr-image-input');
  if (!canvas) return;

  imageInput.onchange = (e) => {
    if (e.target.files.length > 0) {
      const file = e.target.files[0];
      const reader = new FileReader();
      reader.onload = (event) => {
        scrImage = new Image();
        scrImage.onload = () => {
          // Center and scale image to fit canvas bounds initially
          const scaleW = 250 / scrImage.width;
          const scaleH = 122 / scrImage.height;
          scrScale = Math.max(scaleW, scaleH);
          document.getElementById('scr-image-scale').value = scrScale.toFixed(2);
          document.getElementById('scr-scale-val').innerText = scrScale.toFixed(2);
          
          scrImgX = Math.round((250 - scrImage.width * scrScale) / 2);
          scrImgY = Math.round((122 - scrImage.height * scrScale) / 2);
          document.getElementById('scr-image-x').value = scrImgX;
          document.getElementById('scr-image-y').value = scrImgY;
          
          drawScreensaverPreview();
        };
        scrImage.src = event.target.result;
      };
      reader.readAsDataURL(file);
    }
  };

  // Dragging event handlers
  canvas.addEventListener('mousedown', (e) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.round(((e.clientX - rect.left) / rect.width) * 250);
    const y = Math.round(((e.clientY - rect.top) / rect.height) * 122);
    
    scrIsDragging = true;
    scrDragStartX = x;
    scrDragStartY = y;
    e.preventDefault();
  });

  window.addEventListener('mousemove', (e) => {
    if (!scrIsDragging) return;
    const rect = canvas.getBoundingClientRect();
    const x = Math.round(((e.clientX - rect.left) / rect.width) * 250);
    const y = Math.round(((e.clientY - rect.top) / rect.height) * 122);
    
    const dx = x - scrDragStartX;
    const dy = y - scrDragStartY;
    
    scrDragStartX = x;
    scrDragStartY = y;
    
    const dragMode = document.getElementById('scr-drag-mode').value;
    if (dragMode === 'text') {
      scrTextX += dx;
      scrTextY += dy;
      document.getElementById('scr-text-x').value = scrTextX;
      document.getElementById('scr-text-y').value = scrTextY;
    } else {
      scrImgX += dx;
      scrImgY += dy;
      document.getElementById('scr-image-x').value = scrImgX;
      document.getElementById('scr-image-y').value = scrImgY;
    }
    
    drawScreensaverPreview();
  });

  window.addEventListener('mouseup', () => {
    scrIsDragging = false;
  });

  // Touch support for dragging on mobile devices
  canvas.addEventListener('touchstart', (e) => {
    if (e.touches.length === 1) {
      const rect = canvas.getBoundingClientRect();
      const x = Math.round(((e.touches[0].clientX - rect.left) / rect.width) * 250);
      const y = Math.round(((e.touches[0].clientY - rect.top) / rect.height) * 122);
      scrIsDragging = true;
      scrDragStartX = x;
      scrDragStartY = y;
      e.preventDefault();
    }
  });

  canvas.addEventListener('touchmove', (e) => {
    if (scrIsDragging && e.touches.length === 1) {
      const rect = canvas.getBoundingClientRect();
      const x = Math.round(((e.touches[0].clientX - rect.left) / rect.width) * 250);
      const y = Math.round(((e.touches[0].clientY - rect.top) / rect.height) * 122);
      
      const dx = x - scrDragStartX;
      const dy = y - scrDragStartY;
      scrDragStartX = x;
      scrDragStartY = y;
      
      const dragMode = document.getElementById('scr-drag-mode').value;
      if (dragMode === 'text') {
        scrTextX += dx;
        scrTextY += dy;
        document.getElementById('scr-text-x').value = scrTextX;
        document.getElementById('scr-text-y').value = scrTextY;
      } else {
        scrImgX += dx;
        scrImgY += dy;
        document.getElementById('scr-image-x').value = scrImgX;
        document.getElementById('scr-image-y').value = scrImgY;
      }
      drawScreensaverPreview();
      e.preventDefault();
    }
  });

  canvas.addEventListener('touchend', () => {
    scrIsDragging = false;
  });

  drawScreensaverPreview();
}

function updateScrScale(val) {
  scrScale = parseFloat(val);
  document.getElementById('scr-scale-val').innerText = scrScale.toFixed(2);
  drawScreensaverPreview();
}

function updateScrThreshold(val) {
  document.getElementById('scr-threshold-val').innerText = val;
  drawScreensaverPreview();
}

function updateScrImagePosInputs() {
  scrImgX = parseInt(document.getElementById('scr-image-x').value) || 0;
  scrImgY = parseInt(document.getElementById('scr-image-y').value) || 0;
  drawScreensaverPreview();
}

function resetScrImagePos() {
  if (scrImage) {
    const scaleW = 250 / scrImage.width;
    const scaleH = 122 / scrImage.height;
    scrScale = Math.max(scaleW, scaleH);
    document.getElementById('scr-image-scale').value = scrScale.toFixed(2);
    document.getElementById('scr-scale-val').innerText = scrScale.toFixed(2);
    
    scrImgX = Math.round((250 - scrImage.width * scrScale) / 2);
    scrImgY = Math.round((122 - scrImage.height * scrScale) / 2);
  } else {
    scrImgX = 0;
    scrImgY = 0;
  }
  document.getElementById('scr-image-x').value = scrImgX;
  document.getElementById('scr-image-y').value = scrImgY;
  drawScreensaverPreview();
}

function updateScrTextPosInputs() {
  scrTextX = parseInt(document.getElementById('scr-text-x').value) || 0;
  scrTextY = parseInt(document.getElementById('scr-text-y').value) || 0;
  drawScreensaverPreview();
}

function updateScrTextColor(val) {
  scrTextColor = val;
  drawScreensaverPreview();
}

function updateScrTextSize(val) {
  scrTextSize = parseInt(val);
  document.getElementById('scr-text-size-val').innerText = val;
  drawScreensaverPreview();
}

function updateScrTextFont(val) {
  scrTextFont = val;
  drawScreensaverPreview();
}

function thresholdConvert(imgData, threshold) {
  const d = imgData.data;
  const out = new Uint8ClampedArray(250 * 122);
  for (let i = 0; i < d.length; i += 4) {
    const luma = 0.299 * d[i] + 0.587 * d[i+1] + 0.114 * d[i+2];
    out[i/4] = luma < threshold ? 0 : 255;
  }
  return out;
}

function floydSteinbergDither(imgData, threshold) {
  const w = 250;
  const h = 122;
  const d = imgData.data;
  const buffer = new Float32Array(w * h);
  for (let i = 0; i < d.length; i += 4) {
    buffer[i/4] = 0.299 * d[i] + 0.587 * d[i+1] + 0.114 * d[i+2];
  }
  
  const out = new Uint8ClampedArray(w * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const idx = y * w + x;
      const oldPixel = buffer[idx];
      const newPixel = oldPixel < threshold ? 0 : 255;
      out[idx] = newPixel;
      const err = oldPixel - newPixel;
      
      if (x + 1 < w)   buffer[idx + 1] += err * 7 / 16;
      if (y + 1 < h) {
        if (x > 0)     buffer[idx + w - 1] += err * 3 / 16;
        buffer[idx + w] += err * 5 / 16;
        if (x + 1 < w) buffer[idx + w + 1] += err * 1 / 16;
      }
    }
  }
  return out;
}

function drawScreensaverPreview() {
  const canvas = document.getElementById('screensaver-canvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  
  // Clear canvas to white
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, 250, 122);
  
  if (scrImage) {
    // Draw scaled and shifted image on an offscreen canvas
    const offscreen = document.createElement('canvas');
    offscreen.width = 250;
    offscreen.height = 122;
    const oCtx = offscreen.getContext('2d');
    oCtx.fillStyle = '#ffffff';
    oCtx.fillRect(0, 0, 250, 122);
    
    const dw = scrImage.width * scrScale;
    const dh = scrImage.height * scrScale;
    oCtx.drawImage(scrImage, scrImgX, scrImgY, dw, dh);
    
    const imgData = oCtx.getImageData(0, 0, 250, 122);
    const dither = document.getElementById('scr-dither-enable').checked;
    const threshold = parseInt(document.getElementById('scr-image-threshold').value);
    const invert = document.getElementById('scr-invert-enable').checked;
    
    let pixels;
    if (dither) {
      pixels = floydSteinbergDither(imgData, threshold);
    } else {
      pixels = thresholdConvert(imgData, threshold);
    }
    
    // Put binarized pixels back onto main canvas
    const targetData = ctx.createImageData(250, 122);
    for (let i = 0; i < pixels.length; i++) {
      let val = pixels[i];
      if (invert) val = 255 - val;
      const idx = i * 4;
      targetData.data[idx] = val;
      targetData.data[idx+1] = val;
      targetData.data[idx+2] = val;
      targetData.data[idx+3] = 255;
    }
    ctx.putImageData(targetData, 0, 0);
  }
  
  // Draw text overlay on top
  const text = document.getElementById('scr-text-input').value;
  if (text) {
    ctx.font = `${scrTextSize}px ${scrTextFont}`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = scrTextColor === 'white' ? '#ffffff' : '#000000';
    
    // Stroking to ensure readability over complex backgrounds
    ctx.strokeStyle = scrTextColor === 'white' ? '#000000' : '#ffffff';
    ctx.lineWidth = 3;
    ctx.strokeText(text, scrTextX, scrTextY);
    
    ctx.fillText(text, scrTextX, scrTextY);
  }
}

function compileScreensaverBin() {
  const canvas = document.getElementById('screensaver-canvas');
  if (!canvas) return null;
  const ctx = canvas.getContext('2d');
  const imgData = ctx.getImageData(0, 0, 250, 122);
  const d = imgData.data;
  
  const buffer = new Uint8Array(3904); // 32 bytes/row * 122 rows
  for (let y = 0; y < 122; y++) {
    for (let b = 0; b < 32; b++) {
      let byteVal = 0;
      for (let bit = 0; bit < 8; bit++) {
        const x = b * 8 + bit;
        let isWhite = true;
        if (x < 250) {
          const idx = (y * 250 + x) * 4;
          // In screensaver-canvas, rgb(255,255,255) is white, rgb(0,0,0) is black.
          isWhite = (d[idx] >= 128);
        } else {
          // Pad row edge using last pixel color
          const idx = (y * 250 + 249) * 4;
          isWhite = (d[idx] >= 128);
        }
        
        if (isWhite) {
          byteVal |= (1 << bit); // LSB-first: white is 1, black is 0
        }
      }
      buffer[y * 32 + b] = byteVal;
    }
  }
  return buffer;
}

async function uploadScreensaverBin() {
  const bytes = compileScreensaverBin();
  if (!bytes) {
    alert('Failed to process image data.');
    return;
  }
  
  if (!confirm('Upload screensaver to Pala One?')) return;
  
  try {
    const blob = new Blob([bytes], { type: 'application/octet-stream' });
    const formData = new FormData();
    formData.append('file', blob, 'sleep.bin');
    
    const res = await fetch('/upload-sleep', {
      method: 'POST',
      body: formData
    });
    
    if (res.ok) {
      alert('Screensaver uploaded and applied successfully!');
    } else {
      alert('Upload failed: ' + await res.text());
    }
  } catch (e) {
    alert('Upload error: ' + e);
  }
}

function downloadScreensaverBin() {
  const bytes = compileScreensaverBin();
  if (!bytes) return;
  
  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'sleep.bin';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

window.onload = function() {
  loadData();
  checkDragAndDrop();
  initScreensaverDesigner();
};
</script>
</body>
</html>
)rawhtml";
  server.send_P(200, "text/html; charset=utf-8", MAIN_HTML);
}

void handleApiBooks() {
  loadBooks();
  String out = "{\"books\":[";
  for (int i = 0; i < bookCount; i++) {
    if (i > 0) out += ",";
    out += "{";
    out += "\"id\":" + String(i) + ",";
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
  size_t total = FS.totalBytes();
  size_t used = FS.usedBytes();
  size_t booksSize = getFolderSize("/reading");
  size_t freeSize = total - used;
  int pct = (total > 0) ? (used * 100 / total) : 0;

  String out = "{";
  out += "\"fontSize\":" + String(g_fontSize) + ",";
  out += "\"sleepSecs\":" + String(g_sleepSecs) + ",";
  out += "\"lineGap\":" + String(LINE_GAP) + ",";
  out += "\"nightMode\":" + String(g_nightMode ? "true" : "false") + ",";
  out += "\"spotifyScreensaver\":" + String(g_spotifyScreensaver ? "true" : "false") + ",";
  out += "\"spotifyClientId\":\"" + jsonEscape(g_spotifyClientId) + "\",";
  out += "\"spotifyClientSecret\":\"" + jsonEscape(g_spotifyClientSecret) + "\",";
  out += "\"spotifyRefreshToken\":\"" + jsonEscape(g_spotifyRefreshToken) + "\",";
  out += "\"chessElo\":" + String(g_chessElo) + ",";
  out += "\"screensaverMode\":" + String(g_screensaverMode) + ",";
  out += "\"calendarUrl\":\"" + jsonEscape(g_calUrl) + "\",";
  out += "\"timezoneOffset\":" + String(g_timezoneOffsetHours) + ",";
  out += "\"storageTotal\":" + String(total) + ",";
  out += "\"storageUsed\":" + String(used) + ",";
  out += "\"storageBooks\":" + String(booksSize) + ",";
  out += "\"storageFree\":" + String(freeSize) + ",";
  out += "\"storagePct\":" + String(pct);
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
  String fullPath = "/reading/" + folder;
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
  String fullPath = "/reading/" + folder;
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
  String destDir = (folder.length() == 0) ? String("/reading") : String("/reading/") + folder;
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

  if (server.hasArg("scr_mode")) {
    g_screensaverMode = server.arg("scr_mode").toInt();
    prefs.putInt("cfg_scr_mode", g_screensaverMode);
    g_spotifyScreensaver = (g_screensaverMode == 1);
    prefs.putBool("spot_scr", g_spotifyScreensaver);
  }

  if (server.hasArg("cal_url")) {
    g_calUrl = server.arg("cal_url");
    prefs.putString("cfg_cal_url", g_calUrl);
  }

  if (server.hasArg("tz_offset")) {
    g_timezoneOffsetHours = server.arg("tz_offset").toInt();
    prefs.putInt("cfg_tz_offset", g_timezoneOffsetHours);
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
