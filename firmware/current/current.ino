#include <heltec-eink-modules.h>

#include "pala_one_sleep_black_icon_v4.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>

#include <LittleFS.h>
#define FS LittleFS

#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
U8G2_FOR_ADAFRUIT_GFX u8g2;

#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <Update.h>

#include <rom/tjpgd.h>
#include "mbedtls/base64.h"
#include <ArduinoJson.h>

#if ARDUINOJSON_VERSION_MAJOR >= 7
  #define ALLOC_JSON_DOC(name, size) JsonDocument name
#else
  #define ALLOC_JSON_DOC(name, size) DynamicJsonDocument name(size)
#endif

EInkDisplay_WirelessPaperV1_2 display;

// ---------------------- Firmware Version ----------------------
#define FW_VERSION "1.7.7"

static String g_wifiSsid = "";
static String g_wifiPass = "";

// ---------------------- Multi-WiFi Configuration ----------------------
static const int MAX_WIFI_PROFILES = 5;
struct WifiProfile {
  String ssid;
  String pass;
};
static WifiProfile g_wifiProfiles[MAX_WIFI_PROFILES];
static int g_wifiCount = 0;

struct SpotifyTrackInfo {
  bool active = false;
  bool isPlaying = false;
  String trackName;
  String artistName;
  String coverUrl;
};

// ---------------------- Spotify Configuration ----------------------
static String g_spotifyClientId = "";
static String g_spotifyClientSecret = "";
static String g_spotifyRefreshToken = "";
static bool   g_spotifyScreensaver = false;
static bool   g_nightMode = false;

static String g_spotifyAccessToken = "";
static uint32_t g_spotifyTokenTimeMs = 0;

static String g_lastTrackName = "";
static String g_lastArtistName = "";
static bool   g_lastIsPlaying = false;
static bool   g_spotifyForceRefresh = false;

RTC_DATA_ATTR static bool rtc_inSpotifyScreensaver = false;
RTC_DATA_ATTR static uint32_t rtc_lastSpotifyHash = 0;

// ---------------------- Jump Mode & Chapters ----------------------
static bool g_jumpModeActive = false;

struct Chapter {
  uint32_t offset;
  char title[32];
};
static const int MAX_CHAPTERS = 64;
static Chapter g_chapters[MAX_CHAPTERS];
static int g_chapterCount = 0;
static int g_selectedChapterIdx = 0;

// ---------------------- Chess Configuration & State ----------------------
int g_chessElo = 1500;

struct ChessPiecePos {
  int r, c;
  char type; // lowercase
};

struct ChessMove {
  int r1, c1, r2, c2;
};

// Chess Game State
static char g_chessBoard[8][8];
static bool g_chessWhiteToMove = true;
static bool g_chessPlayerIsWhite = true;

// Current Puzzle Details
static String g_chessPuzzleId = "";
static int g_chessPuzzleRating = 0;
static String g_chessSolution[12];
static int g_chessSolutionCount = 0;
static int g_chessCurrentMoveIdx = 0;
static int g_chessWrongTries = 0;
static bool g_chessAlreadyFailed = false;

// Results
static bool g_chessLast10Results[10];
static int g_chessLast10Count = 0;

// Input & Selection State
enum ChessSubMode {
  CHESS_SUB_PIECE_SEL,
  CHESS_SUB_MOVE_SEL,
  CHESS_SUB_RESULT
};
static ChessSubMode g_chessSubMode = CHESS_SUB_PIECE_SEL;

static ChessPiecePos g_chessAvailablePieces[16];
static int g_chessAvailablePiecesCount = 0;
static int g_chessSelectedPieceIdx = 0;

static ChessMove g_chessPossibleMoves[32];
static int g_chessPossibleMovesCount = 0;
static int g_chessSelectedMoveIdx = 0;

// ---------------------- Device / Display ----------------------
static const int W = 250;
static const int H = 122;

// ---------------------- Bookmarks ----------------------
static const uint8_t MAX_BOOKMARKS = 12;

// ---------------------- Battery ----------------------
#define HAS_BATTERY 1
#if HAS_BATTERY
  #define BAT_ADC_CTRL 19
  #define BAT_ADC_IN   20

  static float    g_batVRaw = 0.0f;
  static float    g_batVFiltered = 0.0f;
  static int      g_batPct = 0;
  static int      g_batPctShown = 0;
  static bool     g_batValid = false;
  static bool     g_batLow = false;
  static uint32_t g_batLastMs = 0;
  static const uint32_t BAT_CACHE_MS = 30000;
  static float    g_batCalFactor = 1.00f;
#endif

// ---------------------- Button ----------------------
#define BTN1 0
#define BTN2 33
#define BTN3 37

// â€œFeelâ€ tuning
static const uint32_t DOUBLE_MS   = 240;
static const uint32_t TRIPLE_MS   = 420;
static const uint32_t LONG_MS     = 950;
static const uint32_t DEBOUNCE_MS = 22;

// ---------------------- Modes ----------------------
enum Mode {
  MODE_LIBRARY,
  MODE_READER,
  MODE_UPLOAD,
  MODE_ABOUT,
  MODE_BM_BOOK_SELECT,
  MODE_BM_LIST,
  MODE_BM_PREVIEW,
  MODE_SPOTIFY,
  MODE_SETTINGS,
  MODE_CHESS,
  MODE_TODO,
  MODE_CALENDAR,
  MODE_CHAPTER_LIST
};
Mode mode = MODE_LIBRARY;

// ---------------------- Reader long press action ----------------------
enum ReaderLongPressAction {
  LONGPRESS_BOOKMARK = 0
};
static int g_readerLongPressAction = LONGPRESS_BOOKMARK;

// ---------------------- WiFi Upload AP ----------------------
static char AP_SSID[24] = "PALA-";
static const char* AP_PASS = "palaread";
WebServer server(80);

File uploadFile;
File sleepUploadFile;
String uploadPath;
String uploadPending;

bool   g_uploadOk = false;
String g_uploadError;
String g_uploadFinalName;

bool   g_sleepUploadOk = false;
String g_sleepUploadError;
String g_sleepUploadTmpPath;

// ---------------------- Preferences / FS ----------------------
Preferences prefs;

// ---------------------- UI ----------------------
static const int MARGIN_X = 6;
static const int TOP_PAD  = 0;
static const int BOT_PAD  = 0;

static const bool SHOW_PROGRESS_BAR = true;
static const bool SHOW_PAGE_NUMBER  = true;

static const uint8_t* PAGE_FONT = u8g2_font_5x8_tf;

const uint8_t* MAIN_FONT = u8g2_font_helvR08_tf;
const uint8_t* BOLD_FONT = u8g2_font_helvB08_tf;

static const int STATUS_H = 8;

int LINE_GAP = 0;

static const int FULL_REFRESH_EVERY_N_PAGES = 50;
static const int MENU_FULL_REFRESH_EVERY   = 20;

// Power
static const bool ENABLE_DEEP_SLEEP = true;
uint32_t SLEEP_AFTER_MS = 120 * 1000;

// ---------------------- Books / Library ----------------------
struct BookInfo {
  char   name[80];
  char   path[96];
  size_t size;
  char   folder[64];
};

static const int MAX_BOOKS = 80;
BookInfo books[MAX_BOOKS];
int bookCount = 0;

static const int MAX_FOLDERS = 32;
char folders[MAX_FOLDERS][64];
int folderCount = 0;

int selectedItem = 0;
String currentLibraryFolder;

enum LibraryEntryType {
  LIB_ENTRY_FOLDER,
  LIB_ENTRY_BOOK,
  LIB_ENTRY_BOOKMARKS,
  LIB_ENTRY_TODO,
  LIB_ENTRY_CALENDAR,
  LIB_ENTRY_SPOTIFY,
  LIB_ENTRY_CHESS,
  LIB_ENTRY_SETTINGS
};

static const int MAX_LIBRARY_ENTRIES = MAX_BOOKS + MAX_FOLDERS + 6;
LibraryEntryType libraryEntryTypes[MAX_LIBRARY_ENTRIES];
int libraryEntryRefs[MAX_LIBRARY_ENTRIES];
int libraryEntryCount = 0;

// Reader state
File bookFile;
String currentBookKey;
String currentBookPath;
int pageIndex = 0;

// Pagination offsets
static const int MAX_PAGES = 10000;
uint32_t pageOffsets[MAX_PAGES];
int knownPages = 0;
bool eofReached = false;

// Bookmarks UI state
int bmBookIndex = 0;
int bmSelIndex  = 0;
uint16_t bmPages[MAX_BOOKMARKS];
uint8_t bmCount = 0;

bool bmPreviewActive    = false;
int  bmPreviewSavedPage = 0;

// Render state
uint32_t lastPageStartOffset = 0;
int pageTurnsSinceFull = 0;
int menuDrawsSinceFull = 0;

// Sleep timer
uint32_t lastUserActionMs = 0;

// Toast
String toastMsg;
uint32_t toastUntilMs = 0;
static const uint32_t TOAST_MS = 650;

// ---- Runtime-configurable settings ----
static int      g_fontSize  = 8;
static uint32_t g_sleepSecs = 120;

// Settings UI state
enum SettingsEntryType {
  SET_ENTRY_BACK,
  SET_ENTRY_UPLOAD,
  SET_ENTRY_ABOUT,
  SET_ENTRY_NIGHT_MODE,
  SET_ENTRY_TEXT_SIZE,
  SET_ENTRY_SCREENSAVER,
  SET_ENTRY_MEMORY
};
static const int SETTINGS_COUNT = 7;
static int selectedSettingItem = 1;

static bool g_servicesActive = false;
void startUploadServicesOnly();
void stopUploadServicesOnly();
void drawSettings();
void handleModeSettings();

// ---- Upload mode auto-exit ----
static uint32_t g_uploadStartMs           = 0;
static const uint32_t UPLOAD_AUTO_EXIT_MS = 15UL * 60UL * 1000UL;

// ---- New Screensaver/Clock settings ----
// 0 = Picture, 1 = Spotify, 2 = Chess setup/Daily Board, 3 = Minimalist Clock
static int g_screensaverMode = 0;
RTC_DATA_ATTR static bool rtc_inClockScreensaver = false;
RTC_DATA_ATTR static bool rtc_chessPuzzleSolvedToday = false;

// ---- Checklist/Todo State ----
struct TodoItem {
  bool checked;
  String text;
};
static const int MAX_TODO_ITEMS = 64;
static TodoItem g_todoItems[MAX_TODO_ITEMS];
static int g_todoCount = 0;
static int g_todoSelectedIdx = 0;

// ---- Calendar State ----
struct CalendarEvent {
  String title;
  String dateStr;
  time_t startTime;
};
static const int MAX_CAL_EVENTS = 10;
static CalendarEvent g_calEvents[MAX_CAL_EVENTS];
static int g_calEventCount = 0;
static String g_calUrl = "";
static int g_timezoneOffsetHours = 2; // CEST summer

// ============================================================================
//  GFX adapter
// ============================================================================
class HeltecGFXAdapter : public Adafruit_GFX {
public:
  bool disableInversion = false;
  HeltecGFXAdapter(EInkDisplay_WirelessPaperV1_2 &d) : Adafruit_GFX(W, H), disp(d) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint16_t c;
    if (g_nightMode && !disableInversion) {
      c = (color ? WHITE : BLACK);
    } else {
      c = (color ? BLACK : WHITE);
    }
    int16_t xx = (W - 1) - x;
    int16_t yy = (H - 1) - y;
    disp.drawPixel(xx, yy, c);
  }
private:
  EInkDisplay_WirelessPaperV1_2 &disp;
};
HeltecGFXAdapter gfx(display);

// ============================================================================
//  Layout metrics cache
// ============================================================================
struct LayoutMetrics {
  int ascent;
  int descent;
  int lineH;
  int maxWidth;
  int maxLines;
};

static LayoutMetrics g_metrics;
static bool g_metricsValid = false;

static inline void invalidateMetrics() { g_metricsValid = false; }

static inline const LayoutMetrics& getMetrics() {
  if (!g_metricsValid) {
    u8g2.setFont(MAIN_FONT);

    g_metrics.ascent  = u8g2.getFontAscent();
    g_metrics.descent = u8g2.getFontDescent();
    g_metrics.lineH   = (g_metrics.ascent - g_metrics.descent) + LINE_GAP;

    int w = W - (MARGIN_X * 2);
    if (w < 50) w = 50;
    g_metrics.maxWidth = w;

    int maxHeight = H - TOP_PAD - BOT_PAD;
    if (SHOW_PROGRESS_BAR || SHOW_PAGE_NUMBER) maxHeight -= STATUS_H;

    g_metrics.maxLines = maxHeight / g_metrics.lineH;
    if (g_metrics.maxLines < 1) g_metrics.maxLines = 1;

    g_metricsValid = true;
  }
  return g_metrics;
}

static void applyFontSize(int sz) {
  switch (sz) {
    case  8: MAIN_FONT = u8g2_font_helvR08_tf; BOLD_FONT = u8g2_font_helvB08_tf; break;
    case 10: MAIN_FONT = u8g2_font_helvR10_tf; BOLD_FONT = u8g2_font_helvB10_tf; break;
    case 12: MAIN_FONT = u8g2_font_helvR12_tf; BOLD_FONT = u8g2_font_helvB12_tf; break;
    case 14: MAIN_FONT = u8g2_font_helvR14_tf; BOLD_FONT = u8g2_font_helvB14_tf; break;
    default: MAIN_FONT = u8g2_font_helvR10_tf; BOLD_FONT = u8g2_font_helvB10_tf; sz = 10; break;
  }
  g_fontSize = sz;
  invalidateMetrics();
}

void loadWifiProfiles() {
  g_wifiCount = 0;
  String legacySsid = prefs.getString("cfg_ssid", "");
  String legacyPass = prefs.getString("cfg_pass", "");
  if (legacySsid.length() > 0) {
    g_wifiProfiles[0].ssid = legacySsid;
    g_wifiProfiles[0].pass = legacyPass;
    g_wifiCount = 1;
  }
  
  int storedCount = prefs.getInt("wifi_count", 0);
  if (storedCount > 0) {
    g_wifiCount = 0;
    if (storedCount > MAX_WIFI_PROFILES) storedCount = MAX_WIFI_PROFILES;
    for (int i = 0; i < storedCount; i++) {
      String sKey = "wf_ssid_" + String(i);
      String pKey = "wf_pass_" + String(i);
      String ssid = prefs.getString(sKey.c_str(), "");
      if (ssid.length() > 0) {
        g_wifiProfiles[g_wifiCount].ssid = ssid;
        g_wifiProfiles[g_wifiCount].pass = prefs.getString(pKey.c_str(), "");
        g_wifiCount++;
      }
    }
  }
  
  if (g_wifiCount == 0 && legacySsid.length() > 0) {
    g_wifiProfiles[0].ssid = legacySsid;
    g_wifiProfiles[0].pass = legacyPass;
    g_wifiCount = 1;
  }
  
  if (g_wifiCount > 0) {
    g_wifiSsid = g_wifiProfiles[0].ssid;
    g_wifiPass = g_wifiProfiles[0].pass;
  } else {
    g_wifiSsid = "";
    g_wifiPass = "";
  }
}

void saveWifiProfiles() {
  prefs.putInt("wifi_count", g_wifiCount);
  for (int i = 0; i < g_wifiCount; i++) {
    String sKey = "wf_ssid_" + String(i);
    String pKey = "wf_pass_" + String(i);
    prefs.putString(sKey.c_str(), g_wifiProfiles[i].ssid);
    prefs.putString(pKey.c_str(), g_wifiProfiles[i].pass);
  }
  for (int i = g_wifiCount; i < MAX_WIFI_PROFILES; i++) {
    String sKey = "wf_ssid_" + String(i);
    String pKey = "wf_pass_" + String(i);
    prefs.remove(sKey.c_str());
    prefs.remove(pKey.c_str());
  }
  if (g_wifiCount > 0) {
    prefs.putString("cfg_ssid", g_wifiProfiles[0].ssid);
    prefs.putString("cfg_pass", g_wifiProfiles[0].pass);
    g_wifiSsid = g_wifiProfiles[0].ssid;
    g_wifiPass = g_wifiProfiles[0].pass;
  } else {
    prefs.remove("cfg_ssid");
    prefs.remove("cfg_pass");
    g_wifiSsid = "";
    g_wifiPass = "";
  }
}

static void loadSettings() {
  int fs = prefs.getInt("cfg_font", 8);
  applyFontSize(fs);

  g_sleepSecs = (uint32_t)prefs.getInt("cfg_sleep", 120);
  if (g_sleepSecs < 10)   g_sleepSecs = 10;
  if (g_sleepSecs > 3600) g_sleepSecs = 3600;
  SLEEP_AFTER_MS = g_sleepSecs * 1000UL;

  LINE_GAP = prefs.getInt("cfg_lgap", 0);
  if (LINE_GAP < 0) LINE_GAP = 0;
  if (LINE_GAP > 4) LINE_GAP = 4;

  int saved = prefs.getInt("cfg_lpact", -1);
  if (saved == -1) g_readerLongPressAction = LONGPRESS_BOOKMARK;
  else             g_readerLongPressAction = saved;

  if (g_readerLongPressAction != LONGPRESS_BOOKMARK) {
    g_readerLongPressAction = LONGPRESS_BOOKMARK;
  }

  loadWifiProfiles();

  g_spotifyClientId = prefs.getString("spot_id", "");
  g_spotifyClientSecret = prefs.getString("spot_secret", "");
  g_spotifyRefreshToken = prefs.getString("spot_refresh", "");
  g_spotifyScreensaver = prefs.getBool("spot_scr", false);
  g_nightMode = prefs.getBool("cfg_invert", false);
  g_chessElo = prefs.getInt("cfg_chess_elo", 1500);

  g_screensaverMode = prefs.getInt("cfg_scr_mode", 0);
  if (g_spotifyScreensaver && g_screensaverMode == 0) {
    g_screensaverMode = 1;
  }
  g_calUrl = prefs.getString("cfg_cal_url", "");
  g_timezoneOffsetHours = prefs.getInt("cfg_tz_offset", 2);

  invalidateMetrics();
}

// ============================================================================
//  ISR Button backend
// ============================================================================
static const uint8_t BTN_Q = 64;
static const uint32_t BTN_QUEUE_RECOVER_THRESHOLD = 10;

volatile uint8_t  btnQHead = 0;
volatile uint8_t  btnQTail = 0;
volatile uint8_t  btnQState[BTN_Q];
volatile uint32_t btnQTimeMs[BTN_Q];
volatile uint32_t g_isrDropCount = 0;

static inline uint32_t isrNowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void clearButtonQueue() {
  noInterrupts();
  btnQHead = 0;
  btnQTail = 0;
  interrupts();
}

void IRAM_ATTR btnISR() {
  uint8_t next = (uint8_t)((btnQHead + 1) % BTN_Q);
  if (next == btnQTail) {
    btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q);
    g_isrDropCount++;
  }
  uint8_t state = 0;
  if (digitalRead(BTN1) == LOW) state |= 0x01;
  if (digitalRead(BTN2) == LOW || digitalRead(BTN3) == LOW) state |= 0x02;
  btnQState[btnQHead]  = state;
  btnQTimeMs[btnQHead] = isrNowMs();
  btnQHead = next;
}

// ============================================================================
//  Button handler
// ============================================================================
struct ButtonState {
  bool     stablePressed     = false;
  uint32_t lastStableChange  = 0;
  uint32_t pressStart        = 0;
  bool     pressArmed        = false;
  uint32_t lastRelease       = 0;
  uint32_t firstClickRelease = 0;
  uint8_t  clickCount        = 0;

  bool shortClick  = false;
  bool doubleClick = false;
  bool tripleClick = false;
  bool quadClick   = false;
  bool longClick   = false;

  void resetClicks() {
    shortClick = false;
    doubleClick = false;
    tripleClick = false;
    quadClick = false;
    longClick = false;
  }

  void resetState() {
    stablePressed = false;
    lastStableChange = 0;
    pressStart = 0;
    pressArmed = false;
    lastRelease = 0;
    firstClickRelease = 0;
    clickCount = 0;
    resetClicks();
  }

  void processRaw(bool rawPressed, uint32_t edgeT) {
    if ((uint32_t)(edgeT - lastStableChange) <= DEBOUNCE_MS) return;
    if (rawPressed == stablePressed) return;

    bool prevPressed = stablePressed;
    stablePressed = rawPressed;
    lastStableChange = edgeT;

    if (!prevPressed && stablePressed) {
      pressStart = edgeT;
      pressArmed = true;
    }

    if (prevPressed && !stablePressed) {
      if (pressArmed) {
        uint32_t dur = (uint32_t)(edgeT - pressStart);
        if (dur >= LONG_MS) {
          clickCount = 0;
          longClick = true;
        } else {
          clickCount++;
          lastRelease = edgeT;
          if (clickCount == 1) firstClickRelease = edgeT;
          if (clickCount >= 4) {
            clickCount = 0;
            quadClick = true;
          }
        }
      }
      pressArmed = false;
      pressStart = 0;
    }
  }

  void evaluateClicks(uint32_t now) {
    if (clickCount > 0) {
      bool emit = false;
      if (clickCount <= 2) emit = (uint32_t)(now - lastRelease) > DOUBLE_MS;
      else if (clickCount == 3) emit = (uint32_t)(now - firstClickRelease) > TRIPLE_MS;

      if (emit) {
        if      (clickCount == 1) shortClick = true;
        else if (clickCount == 2) doubleClick = true;
        else if (clickCount == 3) tripleClick = true;
        clickCount = 0;
      }
    }
  }

  bool anyClick() const {
    return shortClick || doubleClick || tripleClick || quadClick || longClick;
  }
};

struct DualButtonState {
  ButtonState b1;
  ButtonState b2;

  void poll() {
    b1.resetClicks();
    b2.resetClicks();

    uint8_t headSnap;
    noInterrupts();
    headSnap = btnQHead;
    interrupts();

    while (btnQTail != headSnap) {
      noInterrupts();
      uint8_t rawState = btnQState[btnQTail];
      uint32_t edgeT  = btnQTimeMs[btnQTail];
      btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q);
      interrupts();

      b1.processRaw((rawState & 0x01) != 0, edgeT);
      b2.processRaw((rawState & 0x02) != 0, edgeT);
    }
    
    uint32_t now = millis();
    b1.evaluateClicks(now);
    b2.evaluateClicks(now);
  }

  bool anyClick() const {
    return b1.anyClick() || b2.anyClick();
  }
};
DualButtonState btns;


// ---- Forward declarations ----
void drawCenter(const char* a, const char* b = nullptr);
#if HAS_BATTERY
void updateBatteryCached(bool force = false);
#endif
void drawBookmarksList();
void drawBookmarksBookSelect();
void drawLibrary();
void drawAbout();
void renderCurrentPage();
void handleModeSpotify();
void startUploadMode();
void stopUploadModeToLibrary();
void goToSleep();
void drawSleepScreen();
void handleResetConfirm();
void handleResetDo();
void handleSettingsPost();
void handleUploadSleepDone();
void handleDeleteSleepImg();
void handleCreateFolder();
void handleMoveBook();
void handleDeleteFolder();
void drawSettings();
void handleModeSettings();
void initChessGame();
void handleModeChess();
void drawChessScreen();
bool fetchChessPuzzle();

static void safeCloseBook();
static void enterLibraryRoot(bool redraw);
static void resetPreviewState();
static void resetUiEphemeralState();
static bool reopenCurrentBookIfNeeded();
static void syncWakeState(bool reading);
static void resetInputFrontend();
static void markUserActivity();

// ---- New Functions for Todo & Calendar ----
void loadTodoList();
void saveTodoList();
void drawTodoList();
void handleModeTodo();

void syncNTP();
bool fetchGoogleCalendar();
void drawCalendarScreen();
void handleModeCalendar();
time_t parseIcsDateTime(const String& dt);
String formatEventTime(time_t t, bool allDay);

void buildTableOfContents();
bool isChapterHeader(const String& s);
void drawChapterList();
void handleModeChapterList();
void jumpToChapter(int idx);

void drawClockScreen();
void drawDailyBoardScreensaver();

// ============================================================================
//  Helpers
// ============================================================================
bool fsBegin() { return FS.begin(true); }

size_t getFolderSize(const String& path) {
  size_t totalSize = 0;
  File dir = FS.open(path);
  if (!dir || !dir.isDirectory()) return 0;
  File f = dir.openNextFile();
  while (f) {
    if (f.isDirectory()) {
      totalSize += getFolderSize(f.path());
    } else {
      totalSize += f.size();
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return totalSize;
}

// ============================================================================
//  Preamble Skipping Helper
// ============================================================================
uint32_t findBookContentStart(File &f) {
  f.seek(0);
  const int bufSize = 4096;
  char* buf = (char*)malloc(bufSize);
  if (!buf) return 0;
  
  int bytesRead = f.read((uint8_t*)buf, bufSize - 1);
  buf[bytesRead] = '\0';
  
  String content(buf);
  free(buf);
  
  int startIdx = content.indexOf("*** START OF THE PROJECT GUTENBERG");
  if (startIdx == -1) startIdx = content.indexOf("*** START OF THIS PROJECT");
  if (startIdx != -1) {
    int endLine = content.indexOf("\n", startIdx);
    if (endLine != -1) {
      while (endLine < content.length() && (content[endLine] == '\r' || content[endLine] == '\n' || content[endLine] == ' ' || content[endLine] == '\t')) {
        endLine++;
      }
      return (uint32_t)endLine;
    }
  }
  return 0;
}

// ============================================================================
//  Spotify Integration Helpers
// ============================================================================
String base64Encode(const String& input) {
  size_t out_len = 0;
  mbedtls_base64_encode(nullptr, 0, &out_len, (const unsigned char*)input.c_str(), input.length());
  uint8_t* buf = (uint8_t*)malloc(out_len + 1);
  mbedtls_base64_encode(buf, out_len, &out_len, (const unsigned char*)input.c_str(), input.length());
  buf[out_len] = '\0';
  String res = String((char*)buf);
  free(buf);
  return res;
}

bool refreshSpotifyToken() {
  if (g_spotifyClientId.length() == 0 || g_spotifyClientSecret.length() == 0 || g_spotifyRefreshToken.length() == 0) {
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    return false;
  }
  String authHeader = "Basic " + base64Encode(g_spotifyClientId + ":" + g_spotifyClientSecret);
  http.addHeader("Authorization", authHeader.c_str());
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + g_spotifyRefreshToken;
  int httpResponseCode = http.POST(body);
  bool success = false;
  if (httpResponseCode == 200) {
    String payload = http.getString();
    int tokenIndex = payload.indexOf("\"access_token\":\"");
    if (tokenIndex != -1) {
      int start = tokenIndex + 16;
      int end = payload.indexOf("\"", start);
      if (end != -1) {
        g_spotifyAccessToken = payload.substring(start, end);
        g_spotifyTokenTimeMs = millis();
        success = true;
      }
    }
  }
  http.end();
  return success;
}

bool sendSpotifyCommand(const String& method, const String& urlPath) {
  if (g_spotifyAccessToken.length() == 0) {
    if (!refreshSpotifyToken()) return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String fullUrl = "https://api.spotify.com" + urlPath;
  if (!http.begin(client, fullUrl.c_str())) return false;
  http.addHeader("Authorization", "Bearer " + g_spotifyAccessToken);
  http.addHeader("Content-Length", "0");
  int httpResponseCode = 0;
  if (method == "POST")          httpResponseCode = http.POST("");
  else if (method == "PUT")     httpResponseCode = http.PUT("");
  else if (method == "GET")     httpResponseCode = http.GET();
  if (httpResponseCode == 401) {
    http.end();
    if (refreshSpotifyToken()) {
      if (http.begin(client, fullUrl.c_str())) {
        http.addHeader("Authorization", "Bearer " + g_spotifyAccessToken);
        http.addHeader("Content-Length", "0");
        if (method == "POST")          httpResponseCode = http.POST("");
        else if (method == "PUT")     httpResponseCode = http.PUT("");
        else if (method == "GET")     httpResponseCode = http.GET();
      }
    }
  }
  http.end();
  return (httpResponseCode >= 200 && httpResponseCode < 300);
}

void spotifyNextTrack() {
  sendSpotifyCommand("POST", "/v1/me/player/next");
}

void spotifyPrevTrack() {
  sendSpotifyCommand("POST", "/v1/me/player/previous");
}

void spotifyTogglePlayPause(bool isPlaying) {
  if (isPlaying) {
    sendSpotifyCommand("PUT", "/v1/me/player/pause");
  } else {
    sendSpotifyCommand("PUT", "/v1/me/player/play");
  }
}

// Spotify Cache globals
static uint8_t g_spotifyCoverCache[1250]; // 100 rows * 12.5 bytes (padded to 13? no, exactly 10000 bits)
static String  g_cachedCoverUrl = "";
static bool    g_spotifyCoverCacheValid = false;

SpotifyTrackInfo getSpotifyCurrentlyPlaying() {
  SpotifyTrackInfo track;
  if (g_spotifyAccessToken.length() == 0) {
    if (!refreshSpotifyToken()) return track;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) {
    return track;
  }
  http.addHeader("Authorization", "Bearer " + g_spotifyAccessToken);
  int httpResponseCode = http.GET();
  if (httpResponseCode == 401) {
    http.end();
    if (refreshSpotifyToken()) {
      if (http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) {
        http.addHeader("Authorization", "Bearer " + g_spotifyAccessToken);
        httpResponseCode = http.GET();
      }
    }
  }
  if (httpResponseCode == 200) {
    String payload = http.getString();
    track.active = true;
    
    auto findValueIdx = [](const String& str, const String& key, int startFrom) -> int {
      int idx = str.indexOf(key, startFrom);
      if (idx == -1) return -1;
      idx += key.length();
      while (idx < (int)str.length()) {
        char c = str[idx];
        if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          idx++;
        } else {
          return idx;
        }
      }
      return -1;
    };

    int ipValIdx = findValueIdx(payload, "\"is_playing\"", 0);
    if (ipValIdx != -1) {
      track.isPlaying = payload.substring(ipValIdx, ipValIdx + 4).startsWith("true");
    }
    
    int itemIdx = payload.indexOf("\"item\"");
    if (itemIdx != -1) {
      int artistsIdx = payload.indexOf("\"artists\"", itemIdx);
      if (artistsIdx != -1) {
        int nameValIdx = findValueIdx(payload, "\"name\"", artistsIdx);
        if (nameValIdx != -1 && payload[nameValIdx] == '"') {
          int end = payload.indexOf("\"", nameValIdx + 1);
          if (end != -1) track.artistName = payload.substring(nameValIdx + 1, end);
        }
      }
      
      int trackTypeIdx = payload.indexOf("\"type\"", itemIdx);
      while(trackTypeIdx != -1) {
         int vIdx = findValueIdx(payload, "\"type\"", trackTypeIdx);
         if (vIdx != -1 && payload[vIdx] == '"' && payload.substring(vIdx + 1, vIdx + 6) == "track") {
             break;
         }
         trackTypeIdx = payload.indexOf("\"type\"", trackTypeIdx + 6);
      }
      
      if (trackTypeIdx != -1) {
         int nameIdx = payload.lastIndexOf("\"name\"", trackTypeIdx);
         if (nameIdx != -1 && nameIdx > itemIdx) {
            int nameValIdx = findValueIdx(payload, "\"name\"", nameIdx);
            if (nameValIdx != -1 && nameValIdx < trackTypeIdx && payload[nameValIdx] == '"') {
               int end = payload.indexOf("\"", nameValIdx + 1);
               if (end != -1) track.trackName = payload.substring(nameValIdx + 1, end);
            }
         }
      }
      
      if (track.trackName.length() == 0) {
         int nameIdx = payload.indexOf("\"name\"", itemIdx);
         if (nameIdx != -1) {
            int nameValIdx = findValueIdx(payload, "\"name\"", nameIdx);
            if (nameValIdx != -1 && payload[nameValIdx] == '"') {
               int end = payload.indexOf("\"", nameValIdx + 1);
               if (end != -1) track.trackName = payload.substring(nameValIdx + 1, end);
            }
         }
      }
      
      int imagesIdx = payload.indexOf("\"images\"", itemIdx);
      if (imagesIdx != -1) {
        int url1Idx = payload.indexOf("\"url\"", imagesIdx);
        if (url1Idx != -1) {
          int url2Idx = payload.indexOf("\"url\"", url1Idx + 5);
          int targetUrlIdx = (url2Idx != -1 && url2Idx < url1Idx + 300) ? url2Idx : url1Idx;
          int urlValIdx = findValueIdx(payload, "\"url\"", targetUrlIdx);
          if (urlValIdx != -1 && payload[urlValIdx] == '"') {
            int end = payload.indexOf("\"", urlValIdx + 1);
            if (end != -1) {
              track.coverUrl = payload.substring(urlValIdx + 1, end);
              track.coverUrl.replace("\\/", "/");
              if (track.coverUrl.startsWith("https://")) {
                track.coverUrl = "https://wsrv.nl/?url=" + track.coverUrl.substring(8) + "&w=100&h=100&output=jpg";
              }
            }
          }
        }
      }
    }
  } else if (httpResponseCode == 204) {
    track.active = false;
  }
  http.end();
  return track;
}

// ============================================================================
//  JPEG Decoder for Spotify Covers
// ============================================================================
struct JpegDecoderState {
  WiFiClient* stream;
  int xOffset;
  int yOffset;
};

static unsigned int jd_input_func(JDEC* jd, uint8_t* buff, unsigned int nbyte) {
  JpegDecoderState* state = (JpegDecoderState*)jd->device;
  if (!state->stream) return 0;
  
  unsigned int readBytes = 0;
  unsigned long startMs = millis();
  while (readBytes < nbyte && (millis() - startMs < 3000)) {
    int bytes = state->stream->read(buff + readBytes, nbyte - readBytes);
    if (bytes > 0) {
      readBytes += bytes;
    } else if (bytes < 0) {
      break; // error
    } else {
      delay(2);
    }
  }
  return readBytes;
}

static UINT jd_output_func(JDEC* jd, void* bitmap, JRECT* rect) {
  JpegDecoderState* state = (JpegDecoderState*)jd->device;
  uint8_t* rgb = (uint8_t*)bitmap;
  int w = rect->right - rect->left + 1;
  int h = rect->bottom - rect->top + 1;
  for (int y = 0; y < h; y++) {
    int imgY = rect->top + y;
    int screenY = state->yOffset + imgY;
    if (screenY >= H) continue;
    for (int x = 0; x < w; x++) {
      int imgX = rect->left + x;
      int screenX = state->xOffset + imgX;
      if (screenX >= W) continue;
      uint8_t r = rgb[(y * w + x) * 3 + 0];
      uint8_t g = rgb[(y * w + x) * 3 + 1];
      uint8_t b = rgb[(y * w + x) * 3 + 2];
      uint8_t luma = (r * 77 + g * 150 + b * 29) >> 8;
      bool isBlack = (luma < 127);
      
      // Draw to GFX screen adapter
      gfx.drawPixel(screenX, screenY, isBlack ? 1 : 0);
      
      // Cache pixels in SRAM (100x100 region)
      if (imgX < 100 && imgY < 100) {
        int bitIdx = imgY * 100 + imgX;
        if (isBlack) {
          g_spotifyCoverCache[bitIdx / 8] |= (1 << (bitIdx % 8));
        } else {
          g_spotifyCoverCache[bitIdx / 8] &= ~(1 << (bitIdx % 8));
        }
      }
    }
  }
  return 1;
}

bool downloadAndDrawSpotifyCover(const String& url, int x, int y) {
  if (url.length() == 0) return false;
  
  // If URL matches cache, draw directly from SRAM
  if (g_spotifyCoverCacheValid && g_cachedCoverUrl == url) {
    for (int cy = 0; cy < 100; cy++) {
      for (int cx = 0; cx < 100; cx++) {
        int bitIdx = cy * 100 + cx;
        bool isBlack = (g_spotifyCoverCache[bitIdx / 8] & (1 << (bitIdx % 8))) != 0;
        gfx.drawPixel(x + cx, y + cy, isBlack ? 1 : 0);
      }
    }
    return true;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url.c_str())) return false;
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  JDEC jd;
  const int poolSize = 8192; // 8KB pool size is safer for web JPEGs
  uint8_t* pool = (uint8_t*)malloc(poolSize);
  if (!pool) {
    http.end();
    return false;
  }
  
  // Clear the SRAM cache buffer before filling it
  memset(g_spotifyCoverCache, 0, sizeof(g_spotifyCoverCache));
  
  JpegDecoderState state;
  state.stream = stream;
  state.xOffset = x;
  state.yOffset = y;
  JRESULT res = jd_prepare(&jd, jd_input_func, pool, poolSize, &state);
  if (res == JDR_OK) {
    res = jd_decomp(&jd, jd_output_func, 0); // scale by 1/1 (100x100 -> 100x100)
  }
  free(pool);
  http.end();
  
  if (res == JDR_OK) {
    g_cachedCoverUrl = url;
    g_spotifyCoverCacheValid = true;
    return true;
  } else {
    g_spotifyCoverCacheValid = false;
    g_cachedCoverUrl = "";
    return false;
  }
}

void drawSpotifyScreen(const SpotifyTrackInfo& track) {
  prepareMenuFrame();
  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }
  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Spotify");
  #if HAS_BATTERY
  drawBatteryTopRight();
  #endif
  gfx.drawFastHLine(MARGIN_X, 15, W - (MARGIN_X * 2), 1);
  if (!track.active) {
    u8g2.setFont(MAIN_FONT);
    u8g2.setCursor(MARGIN_X + 20, 55);
    u8g2.print("Spotify is idle.");
    u8g2.setCursor(MARGIN_X + 20, 75);
    u8g2.print("Play a song to begin!");
  } else {
    int coverX = MARGIN_X + 2;
    int coverY = 22;
    int coverSize = 100;
    gfx.drawRect(coverX - 1, coverY - 1, coverSize + 2, coverSize + 2, 1);
    bool coverLoaded = false;
    if (track.coverUrl.length() > 0) {
      coverLoaded = downloadAndDrawSpotifyCover(track.coverUrl, coverX, coverY);
    }
    if (!coverLoaded) {
      gfx.fillRect(coverX, coverY, coverSize, coverSize, 1);
      gfx.drawRect(coverX + 20, coverY + 20, coverSize - 40, coverSize - 40, 0);
    }
    int txtX = coverX + coverSize + 10;
    int txtW = W - txtX - MARGIN_X;
    u8g2.setFont(BOLD_FONT);
    String title = track.trackName;
    if (u8g2.getUTF8Width(title.c_str()) > txtW) {
      while (title.length() > 0 && u8g2.getUTF8Width((title + "...").c_str()) > txtW) {
        title.remove(title.length() - 1);
      }
      title += "...";
    }
    u8g2.setCursor(txtX, 42);
    u8g2.print(title.c_str());
    u8g2.setFont(MAIN_FONT);
    String artist = track.artistName;
    if (u8g2.getUTF8Width(artist.c_str()) > txtW) {
      while (artist.length() > 0 && u8g2.getUTF8Width((artist + "...").c_str()) > txtW) {
        artist.remove(artist.length() - 1);
      }
      artist += "...";
    }
    u8g2.setCursor(txtX, 60);
    u8g2.print(artist.c_str());
    int iconX = txtX;
    int iconY = 72;
    if (track.isPlaying) {
      gfx.fillTriangle(iconX, iconY, iconX + 8, iconY + 4, iconX, iconY + 8, 1);
      u8g2.setFont(u8g2_font_5x8_tf);
      u8g2.setCursor(iconX + 14, iconY + 7);
      u8g2.print("Playing");
    } else {
      gfx.fillRect(iconX, iconY, 3, 8, 1);
      gfx.fillRect(iconX + 4, iconY, 3, 8, 1);
      u8g2.setFont(u8g2_font_5x8_tf);
      u8g2.setCursor(iconX + 14, iconY + 7);
      u8g2.print("Paused");
    }
  }
  u8g2.setFont(u8g2_font_5x8_tf);
  if (track.active) {
    int ctrlX = MARGIN_X + 112; // Right of the cover
    u8g2.setCursor(ctrlX, 96);
    u8g2.print("1x Skip  2x Prev");
    u8g2.setCursor(ctrlX, 108);
    u8g2.print("Hold Pause 3x Exit");
  } else {
    u8g2.setCursor(MARGIN_X, H - 2);
    u8g2.print("1x Skip  2x Prev  Hold Pause  3x Exit");
  }
  display.update();
}

bool connectSTAWithMulti() {
  loadWifiProfiles();
  if (g_wifiCount == 0) return false;

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  esp_wifi_start();
  delay(100);

  WiFiMulti wifiMulti;
  for (int i = 0; i < g_wifiCount; i++) {
    wifiMulti.addAP(g_wifiProfiles[i].ssid.c_str(), g_wifiProfiles[i].pass.c_str());
  }
  unsigned long startMs = millis();
  while (millis() - startMs < 15000) {
    if (wifiMulti.run() == WL_CONNECTED) {
      if (WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        return true;
      }
    }
    delay(250);
  }
  return (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0));
}

bool ensureSpotifyWiFi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
  drawCenter("Spotify", "Connecting to Wi-Fi...");
  return connectSTAWithMulti();
}

void exitSpotifyMode() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  esp_wifi_stop();
  mode = MODE_LIBRARY;
  resetInputFrontend();
  drawLibrary();
}

bool isUsbConnected() {
  #if HAS_BATTERY
    updateBatteryCached(true);
    return g_batVRaw > 4.22f;
  #else
    return true;
  #endif
}

static void safeCloseBook() {
  if (bookFile) bookFile.close();
}

static void resetPreviewState() {
  bmPreviewActive = false;
  bmPreviewSavedPage = 0;
}

static void resetUiEphemeralState() {
  toastMsg = "";
  toastUntilMs = 0;
  resetPreviewState();
}

static void resetNavigationState() {
  currentLibraryFolder = "";
  selectedItem = 0;
  bmBookIndex = 0;
  bmSelIndex = 0;
}

static bool isReaderLikeMode() {
  return mode == MODE_READER || mode == MODE_BM_PREVIEW;
}

static void markUserActivity() {
  lastUserActionMs = millis();
}

static void syncWakeState(bool reading) {
  prefs.putInt("wake_mode", reading ? 1 : 0);
  if (reading && currentBookPath.length() > 0) prefs.putString("wake_path", currentBookPath);
  else prefs.remove("wake_path");
}

static void enterLibraryRoot(bool redraw = true) {
  safeCloseBook();
  resetPreviewState();
  resetNavigationState();
  syncWakeState(false);
  mode = MODE_LIBRARY;
  if (redraw) drawLibrary();
}

static void resetInputFrontend() {
  while (digitalRead(BTN1) == LOW) delay(5);
  delay(DEBOUNCE_MS + 8);
  clearButtonQueue();
  btns.b1.resetState();
  markUserActivity();
}

static bool reopenCurrentBookIfNeeded() {
  if (currentBookPath.length() == 0) return false;
  safeCloseBook();
  bookFile = FS.open(currentBookPath, "r");
  return (bool)bookFile;
}

void ensureBooksDir() {
  if (!FS.exists("/reading")) FS.mkdir("/reading");
}

static String stripTxtExt(const String& s) {
  if (s.endsWith(".txt")) return s.substring(0, s.length() - 4);
  return s;
}

static String bmKeyFor(const String& bookKey);
String prefKeyForBook(const String& path);

static String lastPathComponent(const String& path) {
  int slash = path.lastIndexOf('/');
  return (slash >= 0) ? path.substring(slash + 1) : path;
}

static String prettyRelativeLabel(const String& relPath) {
  String out;
  out.reserve(relPath.length() + 8);
  for (size_t i = 0; i < relPath.length(); i++) {
    char c = relPath[i];
    if (c == '_') out += ' ';
    else if (c == '/') out += " / ";
    else out += c;
  }
  return stripTxtExt(out);
}

static bool isAllowedFolderByte(uint8_t c) {
  return ((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == ' ' ||
          c >= 128);
}

static String sanitizeFolderSegment(const String& segment) {
  String clean;
  clean.reserve(segment.length());
  for (size_t i = 0; i < segment.length(); i++) {
    uint8_t c = (uint8_t)segment[i];
    if (isAllowedFolderByte(c)) clean += (char)c;
    else clean += '_';
  }
  clean.trim();
  return clean;
}

static String sanitizeFolderInput(const String& raw) {
  String normalized = raw;
  normalized.replace('\\', '/');

  String out;
  int start = 0;
  while (start <= normalized.length()) {
    int slash = normalized.indexOf('/', start);
    String part = (slash >= 0) ? normalized.substring(start, slash)
                               : normalized.substring(start);
    start = (slash >= 0) ? (slash + 1) : (normalized.length() + 1);

    part.trim();
    if (part.length() == 0 || part == "." || part == "..") continue;

    String clean = sanitizeFolderSegment(part);
    if (clean.length() == 0) continue;

    if (out.length() > 0) out += '/';
    out += clean;
  }
  return out;
}

static String sanitizeUploadedFilename(String fname) {
  int slash = fname.lastIndexOf('/');
  if (slash >= 0) fname = fname.substring(slash + 1);

  String clean;
  clean.reserve(fname.length());

  for (size_t i = 0; i < fname.length(); i++) {
    uint8_t c = (uint8_t)fname[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == ' ' || c == '.' ||
        c >= 128) {
      clean += (char)c;
    } else {
      clean += '_';
    }
  }

  clean.replace("..", "");
  while (clean.startsWith(".")) clean.remove(0, 1);
  if (!clean.endsWith(".txt")) clean += ".txt";
  if (clean.length() == 0) clean = "book.txt";
  return clean;
}

static bool ensureDirRecursive(const String& path) {
  if (path.length() == 0 || path == "/") return true;
  if (FS.exists(path)) return true;

  int slash = path.lastIndexOf('/');
  if (slash > 0) {
    String parent = path.substring(0, slash);
    if (parent.length() > 0 && !FS.exists(parent)) {
      if (!ensureDirRecursive(parent)) return false;
    }
  }
  return FS.mkdir(path);
}

static bool isDirEmpty(const String& path) {
  File dir = FS.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  File f = dir.openNextFile();
  bool empty = !f;
  if (f) f.close();
  dir.close();
  return empty;
}

static void addFolderIfMissing(const String& folderRel) {
  if (folderRel.length() == 0) return;
  for (int i = 0; i < folderCount; i++) {
    if (strcmp(folders[i], folderRel.c_str()) == 0) return;
  }
  if (folderCount < MAX_FOLDERS) {
    strncpy(folders[folderCount], folderRel.c_str(), 63);
    folders[folderCount][63] = '\0';
    folderCount++;
  }
}

static void sortFolders() {
  for (int i = 0; i < folderCount - 1; i++) {
    for (int j = i + 1; j < folderCount; j++) {
      if (strcmp(folders[j], folders[i]) < 0) {
        char tmp[64];
        memcpy(tmp, folders[i], 64);
        memcpy(folders[i], folders[j], 64);
        memcpy(folders[j], tmp, 64);
      }
    }
  }
}

static void sortBooks() {
  for (int i = 0; i < bookCount - 1; i++) {
    for (int j = i + 1; j < bookCount; j++) {
      if (strcmp(books[j].name, books[i].name) < 0) {
        BookInfo tmp = books[i];
        books[i] = books[j];
        books[j] = tmp;
      }
    }
  }
}

static String folderParent(const String& relPath) {
  int slash = relPath.lastIndexOf('/');
  if (slash < 0) return "";
  return relPath.substring(0, slash);
}

static String folderLeafLabel(const String& relPath) {
  String leaf = lastPathComponent(relPath);
  leaf.replace('_', ' ');
  return leaf;
}

static String bookLeafLabel(int idx) {
  String leaf = stripTxtExt(lastPathComponent(String(books[idx].path)));
  leaf.replace('_', ' ');
  return leaf;
}

static bool libraryFolderExists(const String& folderRel) {
  if (folderRel.length() == 0) return true;
  for (int i = 0; i < folderCount; i++) {
    if (strcmp(folders[i], folderRel.c_str()) == 0) return true;
  }
  for (int i = 0; i < bookCount; i++) {
    if (strcmp(books[i].folder, folderRel.c_str()) == 0) return true;
  }
  return false;
}

static String libraryEntryLabel(int idx) {
  if (idx < 0 || idx >= libraryEntryCount) return "";
  switch (libraryEntryTypes[idx]) {
    case LIB_ENTRY_FOLDER:    return folderLeafLabel(String(folders[libraryEntryRefs[idx]]));
    case LIB_ENTRY_BOOK: {
      int ref = libraryEntryRefs[idx];
      String label = bookLeafLabel(ref);
      String key = prefKeyForBook(String(books[ref].path));
      int pct = prefs.getInt((key + "_pct").c_str(), -1);
      if (pct >= 0) {
        label = "[" + String(pct) + "%] " + label;
      }
      return label;
    }
    case LIB_ENTRY_BOOKMARKS: return "Bookmarks";
    case LIB_ENTRY_TODO:      return "Todo List";
    case LIB_ENTRY_CALENDAR:  return "Calendar";
    case LIB_ENTRY_SPOTIFY:   return "Spotify";
    case LIB_ENTRY_SETTINGS:  return "Settings";
    case LIB_ENTRY_CHESS:     return "Chess";
  }
  return "";
}

static void buildLibraryEntries() {
  while (currentLibraryFolder.length() > 0 && !libraryFolderExists(currentLibraryFolder)) {
    currentLibraryFolder = folderParent(currentLibraryFolder);
  }

  libraryEntryCount = 0;

  for (int i = 0; i < folderCount && libraryEntryCount < MAX_LIBRARY_ENTRIES; i++) {
    if (folderParent(String(folders[i])) == currentLibraryFolder) {
      libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_FOLDER;
      libraryEntryRefs[libraryEntryCount] = i;
      libraryEntryCount++;
    }
  }

  for (int i = 0; i < bookCount && libraryEntryCount < MAX_LIBRARY_ENTRIES; i++) {
    if (strcmp(books[i].folder, currentLibraryFolder.c_str()) == 0) {
      libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_BOOK;
      libraryEntryRefs[libraryEntryCount] = i;
      libraryEntryCount++;
    }
  }

  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_BOOKMARKS;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_TODO;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_CALENDAR;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_SPOTIFY;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_CHESS;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_SETTINGS;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }

  if (selectedItem < 0) selectedItem = 0;
  if (selectedItem >= libraryEntryCount) selectedItem = max(0, libraryEntryCount - 1);
}

static void clearCurrentBookState() {
  if (bookFile) bookFile.close();
  currentBookPath = "";
  currentBookKey  = "";
  pageIndex = 0;
  knownPages = 0;
  eofReached = false;
}

static void migrateBookMetadata(const String& oldPath, const String& newPath) {
  String oldKey = prefKeyForBook(oldPath);
  String newKey = prefKeyForBook(newPath);

  int progress = prefs.getInt((oldKey + "_p").c_str(), -1);
  if (progress >= 0) {
    prefs.putInt((newKey + "_p").c_str(), progress);
    prefs.remove((oldKey + "_p").c_str());
  }

  uint8_t buf[1 + MAX_BOOKMARKS * 2] = {0};
  size_t got = prefs.getBytes(bmKeyFor(oldKey).c_str(), buf, sizeof(buf));
  if (got > 0) {
    prefs.putBytes(bmKeyFor(newKey).c_str(), buf, sizeof(buf));
    prefs.remove(bmKeyFor(oldKey).c_str());
  }

  if (prefs.getString("wake_path", "") == oldPath) {
    prefs.putString("wake_path", newPath);
  }

  if (currentBookPath == oldPath) {
    currentBookPath = newPath;
    currentBookKey = newKey;
  }
}

static void scanBooksRecursive(const String& absDir, const String& relDir) {
  File dir = FS.open(absDir);
  if (!dir || !dir.isDirectory()) return;

  File f = dir.openNextFile();
  while (f) {
    String entryName = String(f.name());
    String absPath;
    if (entryName.startsWith("/")) {
      absPath = entryName;
    } else {
      absPath = absDir;
      if (!absPath.endsWith("/")) absPath += "/";
      absPath += entryName;
    }
    String leaf = lastPathComponent(absPath);

    if (f.isDirectory()) {
      String childRel = relDir.length() ? (relDir + "/" + leaf) : leaf;
      addFolderIfMissing(childRel);
      scanBooksRecursive(absPath, childRel);
    } else if (bookCount < MAX_BOOKS && absPath.endsWith(".txt")) {
      String relFile = relDir.length() ? (relDir + "/" + leaf) : leaf;
      strncpy(books[bookCount].path, absPath.c_str(), 95);
      books[bookCount].path[95] = '\0';

      strncpy(books[bookCount].folder, relDir.c_str(), 63);
      books[bookCount].folder[63] = '\0';

      String _n = prettyRelativeLabel(relFile);
      strncpy(books[bookCount].name, _n.c_str(), 79);
      books[bookCount].name[79] = '\0';

      books[bookCount].size = f.size();
      bookCount++;
    }

    f.close();
    f = dir.openNextFile();
  }

  dir.close();
}

static uint32_t fnv1a32(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619u;
  }
  return h;
}

String prefKeyForBook(const String& path) {
  uint32_t h = fnv1a32(path.c_str());
  char buf[16];
  snprintf(buf, sizeof(buf), "b_%08lx", (unsigned long)h);
  return String(buf);
}

static String normalizeTypography(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  size_t i = 0;

  while (i < in.length()) {
    uint8_t b0 = (uint8_t)in[i];

    if (b0 == 0xC2 && i + 1 < in.length()) {
      uint8_t b1 = (uint8_t)in[i + 1];
      if (b1 == 0xAB || b1 == 0xBB) { out += '\"'; i += 2; continue; }
      if (b1 == 0x91 || b1 == 0x92) { out += '\''; i += 2; continue; }
      if (b1 == 0x93 || b1 == 0x94) { out += '\"'; i += 2; continue; }
      if (b1 == 0x82) { out += '\''; i += 2; continue; }
      if (b1 == 0x84) { out += '\"'; i += 2; continue; }
    }

    if (b0 == 0xE2 && i + 2 < in.length()) {
      uint8_t b1 = (uint8_t)in[i + 1];
      uint8_t b2 = (uint8_t)in[i + 2];
      if (b1 == 0x80) {
        if (b2 == 0x98 || b2 == 0x99) { out += '\''; i += 3; continue; }
        if (b2 == 0x9C || b2 == 0x9D) { out += '\"'; i += 3; continue; }
        if (b2 == 0x9A || b2 == 0x9B) { out += '\''; i += 3; continue; }
        if (b2 == 0x9E || b2 == 0x9F) { out += '\"'; i += 3; continue; }
        if (b2 == 0x93 || b2 == 0x94 || b2 == 0x95) { out += '-'; i += 3; continue; }
        if (b2 == 0xA6) { out += "..."; i += 3; continue; }
        if (b2 == 0xB9 || b2 == 0xBA) { out += '\"'; i += 3; continue; }
      }
    }

    out += (char)b0;
    i++;
  }

  return out;
}

static void showToast(const String& msg) {
  toastMsg = msg;
  toastUntilMs = millis() + TOAST_MS;
}

static inline bool isBookmarkLabelWordChar(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         ((uint8_t)c >= 128);
}

static String readBookmarkLabelAtOffset(File &f, uint32_t off, int page) {
  if (!f.seek(off)) return String("p. ") + String(page + 1);

  String label;
  label.reserve(80);

  const int maxWords = 5;
  const int maxChars = 44;
  int words = 0;
  int scanned = 0;
  bool inWord = false;
  bool pendingSpace = false;

  while (f.available() && scanned < 240) {
    char c = (char)f.read();
    scanned++;

    if (c == '\r') continue;
    if (c == '\n' || c == '\t') c = ' ';

    if (isBookmarkLabelWordChar(c)) {
      if (!inWord) {
        if (words >= maxWords) break;
        if (pendingSpace && label.length() > 0 && label.length() < maxChars) label += ' ';
        pendingSpace = false;
        inWord = true;
      }
      if (label.length() < maxChars) label += c;
      continue;
    }

    if (c == ' ') {
      if (inWord) {
        words++;
        inWord = false;
        pendingSpace = (words < maxWords);
        if (words >= maxWords) break;
      }
      continue;
    }

    if (inWord && label.length() < maxChars) label += c;
  }

  if (inWord) words++;
  label.trim();
  if (label.length() == 0) label = "Page";
  label += " - p. ";
  label += String(page + 1);
  return label;
}

static inline bool isAsciiOnly(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    if ((uint8_t)s[i] >= 128) return false;
  }
  return true;
}

// ============================================================================
//  Progress saving
// ============================================================================
static uint32_t g_lastSaveMs = 0;
static int      g_lastSavedPage = -1;
static const uint32_t SAVE_EVERY_MS = 7000;

static inline void resetSaveThrottle() {
  g_lastSaveMs = 0;
  g_lastSavedPage = -1;
}

void saveProgressThrottled(bool force = false) {
  if (currentBookKey.length() == 0) return;

  if (!force) {
    if (pageIndex == g_lastSavedPage) return;
    uint32_t now = millis();
    if (g_lastSaveMs != 0 && (now - g_lastSaveMs) < SAVE_EVERY_MS) return;
  }

  prefs.putInt((currentBookKey + "_p").c_str(), pageIndex);

  if (bookFile) {
    size_t total = bookFile.size();
    int pct = (total > 0) ? (int)((lastPageStartOffset * 100) / total) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    prefs.putInt((currentBookKey + "_pct").c_str(), pct);
  }

  g_lastSaveMs = millis();
  g_lastSavedPage = pageIndex;
}

void saveProgress() {
  saveProgressThrottled(true);
}

// ============================================================================
//  Battery
// ============================================================================
#if HAS_BATTERY

static inline void adcSetupOnce() {
  pinMode(BAT_ADC_IN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_IN, ADC_11db);
}

static int cmpUint16(const void* a, const void* b) {
  uint16_t aa = *(const uint16_t*)a;
  uint16_t bb = *(const uint16_t*)b;
  if (aa < bb) return -1;
  if (aa > bb) return 1;
  return 0;
}

static inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static uint32_t readAdcMilliVoltsStable() {
  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, LOW);
  delay(12);

  (void)analogReadMilliVolts(BAT_ADC_IN);
  delay(3);
  (void)analogReadMilliVolts(BAT_ADC_IN);
  delay(3);

  const int N = 21;
  uint16_t vals[N];
  for (int i = 0; i < N; i++) {
    vals[i] = (uint16_t)analogReadMilliVolts(BAT_ADC_IN);
    delay(2);
  }

  pinMode(BAT_ADC_CTRL, INPUT);

  qsort(vals, N, sizeof(vals[0]), cmpUint16);

  uint32_t sum = 0;
  for (int i = 3; i < (N - 3); i++) sum += vals[i];
  return sum / (uint32_t)(N - 6);
}

static float readBatteryVoltageRaw() {
  uint32_t mv = readAdcMilliVoltsStable();
  float v = ((float)mv / 1000.0f) * 2.0f;
  v *= g_batCalFactor;
  return v;
}

static int batteryPercentFromOCV(float v) {
  struct BatPoint { float v; int pct; };
  static const BatPoint lut[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85},
    {4.05f, 80},  {4.02f, 75}, {3.99f, 70}, {3.96f, 62},
    {3.93f, 55},  {3.90f, 48}, {3.87f, 40}, {3.84f, 32},
    {3.81f, 24},  {3.78f, 18}, {3.75f, 13}, {3.72f, 9},
    {3.69f, 6},   {3.65f, 4},  {3.55f, 2},  {3.40f, 0}
  };

  if (v >= lut[0].v) return 100;
  const int n = (int)(sizeof(lut) / sizeof(lut[0]));
  if (v <= lut[n - 1].v) return 0;

  for (int i = 0; i < n - 1; i++) {
    float vHi = lut[i].v;
    float vLo = lut[i + 1].v;
    int pHi = lut[i].pct;
    int pLo = lut[i + 1].pct;

    if (v <= vHi && v >= vLo) {
      float t = (v - vLo) / (vHi - vLo);
      int pct = (int)(pLo + t * (float)(pHi - pLo) + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      return pct;
    }
  }
  return 0;
}

void updateBatteryCached(bool force) {
  uint32_t now = millis();
  if (!force && (now - g_batLastMs) < BAT_CACHE_MS) return;
  g_batLastMs = now;

  float raw = readBatteryVoltageRaw();
  bool valid = (raw > 2.8f && raw < 4.5f);
  g_batValid = valid;
  if (!valid) return;

  g_batVRaw = raw;

  if (g_batVFiltered <= 0.0f) {
    g_batVFiltered = raw;
  } else {
    const float alpha = 0.22f;
    g_batVFiltered = (alpha * raw) + ((1.0f - alpha) * g_batVFiltered);
  }
  g_batVFiltered = clampf(g_batVFiltered, 3.0f, 4.25f);

  int pctRaw = batteryPercentFromOCV(g_batVFiltered);
  if (pctRaw < 0) pctRaw = 0;
  if (pctRaw > 100) pctRaw = 100;
  g_batPct = pctRaw;

  if (force) {
    g_batPctShown = g_batPct;
  } else {
    if (g_batPct < g_batPctShown) {
      if ((g_batPctShown - g_batPct) >= 1) g_batPctShown--;
    } else if (g_batPct > g_batPctShown + 2) {
      g_batPctShown++;
    }
  }

  if (g_batPctShown < 0) g_batPctShown = 0;
  if (g_batPctShown > 100) g_batPctShown = 100;

  if (!g_batLow && g_batPctShown <= 8) g_batLow = true;
  else if (g_batLow && g_batPctShown >= 12) g_batLow = false;
}

static void drawBatteryTopRight() {
  updateBatteryCached(false);

  int pct = g_batValid ? g_batPctShown : 0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  const int iconW = 18;
  const int iconH = 9;
  int xIcon = W - MARGIN_X - iconW - 2;
  int yIcon = 2;

  gfx.drawRect(xIcon, yIcon, iconW, iconH, 1);
  gfx.fillRect(xIcon + iconW, yIcon + 2, 2, iconH - 4, 1);

  int innerW = iconW - 2;
  int fillW = (innerW * pct) / 100;
  if (fillW > 0) gfx.fillRect(xIcon + 1, yIcon + 1, fillW, iconH - 2, 1);

  if (g_batLow && pct > 0) {
    gfx.drawLine(xIcon + 3, yIcon + 2, xIcon + 3, yIcon + iconH - 3, 0);
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  char buf[8];
  if (g_batValid) snprintf(buf, sizeof(buf), "%d%%", pct);
  else            snprintf(buf, sizeof(buf), "--");
  int wTxt = u8g2.getUTF8Width(buf);
  int xTxt = xIcon - 4 - wTxt;
  int yTxt = yIcon + 8;
  u8g2.setCursor(xTxt, yTxt);
  u8g2.print(buf);
  u8g2.setFont(MAIN_FONT);
}
#endif

// ============================================================================
//  Drawing primitives
// ============================================================================
void beginPageCanvas(bool clearMem = true) {
  if (clearMem) display.clearMemory();
  display.landscape();
  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(1);
  u8g2.setBackgroundColor(0);
}

void prepareMenuFrame() {
  bool doFull = (menuDrawsSinceFull >= MENU_FULL_REFRESH_EVERY);
  if (doFull) {
    display.fastmodeOff();
    display.clear();
    menuDrawsSinceFull = 0;
  } else {
    display.fastmodeOn();
  }
  beginPageCanvas();
  menuDrawsSinceFull++;
}

static void drawToastIfActive() {
  if (toastUntilMs == 0) return;
  if ((int32_t)(millis() - toastUntilMs) > 0) {
    toastUntilMs = 0;
    toastMsg = "";
    return;
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  int y = H - STATUS_H - 2;
  int x = MARGIN_X;
  u8g2.setCursor(x, y);
  u8g2.print(toastMsg.c_str());
  u8g2.setFont(MAIN_FONT);
}

void drawCenter(const char* a, const char* b) {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int lineH = 16;

  int y = (H / 2) - lineH / 2;
  if (b) y -= lineH / 2;

  int wA = u8g2.getUTF8Width(a);
  u8g2.setCursor((W - wA) / 2, y);
  u8g2.print(a);

  if (b) {
    y += lineH;
    int wB = u8g2.getUTF8Width(b);
    u8g2.setCursor((W - wB) / 2, y);
    u8g2.print(b);
  }
  display.update();
}

// ============================================================================
//  Text layout core
// ============================================================================
uint32_t readPageFromFile(File &f, uint32_t startPos, bool draw, String *outText) {
  f.seek(startPos);

  u8g2.setFont(MAIN_FONT);
  const LayoutMetrics& m = getMetrics();

  int cursorY = TOP_PAD + m.ascent;
  int linesUsed = 0;

  String line = "";
  String word = "";
  line.reserve(96);
  word.reserve(48);

  uint32_t lineStartPos = startPos;
  uint32_t wordStartPos = startPos;

  auto flushLine = [&](const String& toPrint) {
    if (draw) {
      u8g2.setCursor(MARGIN_X, cursorY);
      u8g2.print(toPrint.c_str());
      cursorY += m.lineH;
    }
    if (outText) {
      String t = toPrint;
      t.trim();
      (*outText) += t;
      (*outText) += "\n";
    }
    linesUsed++;
  };

  auto safeReturn = [&](uint32_t off) -> uint32_t {
    if (off <= startPos) off = startPos + 1;
    size_t sz = f.size();
    if (sz > 0 && off > sz) off = sz;
    return off;
  };

  auto utf8CharLen = [](uint8_t b) -> int {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
  };

  auto hardBreakWord = [&](String &w, uint32_t &wStartPos) {
    while (w.length() > 0) {
      String chunk = "";
      chunk.reserve(32);
      int i = 0;
      while (i < (int)w.length()) {
        int clen = utf8CharLen((uint8_t)w[i]);
        if (i + clen > (int)w.length()) clen = (int)w.length() - i;
        String candidate = chunk + w.substring(i, i + clen);
        if (u8g2.getUTF8Width(candidate.c_str()) > m.maxWidth) break;
        chunk = candidate;
        i += clen;
      }
      if (chunk.length() == 0) {
        int clen = utf8CharLen((uint8_t)w[0]);
        if (clen > (int)w.length()) clen = w.length();
        chunk = w.substring(0, clen);
      }
      flushLine(chunk);
      if (linesUsed >= m.maxLines) {
        return safeReturn(wStartPos + (uint32_t)chunk.length());
      }
      w.remove(0, chunk.length());
      wStartPos += (uint32_t)chunk.length();
    }
    return (uint32_t)0;
  };

  while (f.available() && linesUsed < m.maxLines) {
    uint32_t charPos = f.position();
    char c = (char)f.read();

    if (c == '\r') continue;

    if (word.length() == 0 && c != ' ' && c != '\t' && c != '\n') wordStartPos = charPos;

    if (c == '\n' || c == ' ' || c == '\t') {
      if (word.length() > 0) {
        if (line.length() == 0 && isAsciiOnly(word) && u8g2.getUTF8Width(word.c_str()) > m.maxWidth) {
          uint32_t forcedNext = hardBreakWord(word, wordStartPos);
          if (forcedNext != 0) return forcedNext;
          line = "";
          lineStartPos = f.position();
        } else {
          String candidate = line + word + " ";
          int tw = u8g2.getUTF8Width(candidate.c_str());
          if (tw > m.maxWidth) {
            flushLine(line);
            if (linesUsed >= m.maxLines) return safeReturn(wordStartPos);
            line = word + " ";
            lineStartPos = wordStartPos;
          } else {
            line = candidate;
          }
          word = "";
        }
      }

      if (c == '\n') {
        flushLine(line);
        if (linesUsed >= m.maxLines) return safeReturn(f.position());
        line = "";
        lineStartPos = f.position();
      }
    } else {
      word += c;
    }
  }

  if (linesUsed >= m.maxLines) {
    uint32_t next;
    if (word.length() > 0) next = wordStartPos;
    else if (line.length() > 0) next = lineStartPos;
    else next = f.position();
    return safeReturn(next);
  }

  if (linesUsed < m.maxLines && (line.length() || word.length())) {
    if (line.length() == 0 && word.length() > 0 && isAsciiOnly(word) && u8g2.getUTF8Width(word.c_str()) > m.maxWidth) {
      uint32_t forcedNext = hardBreakWord(word, wordStartPos);
      if (forcedNext != 0) return forcedNext;
    } else {
      flushLine(line + word);
    }
  }

  return safeReturn(f.position());
}

// ============================================================================
//  Pagination
// ============================================================================
uint32_t buildNextOffsetFor(File &f, uint32_t startPos) {
  return readPageFromFile(f, startPos, false, nullptr);
}

uint32_t buildNextOffset(uint32_t startPos) {
  uint32_t next = readPageFromFile(bookFile, startPos, false, nullptr);
  if (!bookFile.available()) eofReached = true;
  return next;
}

void ensureOffsetsUpTo(int targetPage) {
  while (!eofReached && knownPages <= targetPage && knownPages < MAX_PAGES) {
    uint32_t start = pageOffsets[knownPages - 1];
    uint32_t next  = buildNextOffset(start);
    if (next <= start) {
      eofReached = true;
      break;
    }
    pageOffsets[knownPages++] = next;
  }
  if (pageIndex >= knownPages) pageIndex = knownPages - 1;
  if (pageIndex < 0) pageIndex = 0;
}

// ============================================================================
//  Books
// ============================================================================
void loadBooks() {
  bookCount = 0;
  folderCount = 0;
  ensureBooksDir();
  scanBooksRecursive("/reading", "");
  sortFolders();
  sortBooks();

  int maxItem = bookCount + 2;
  if (selectedItem < 0) selectedItem = 0;
  if (selectedItem > maxItem) selectedItem = maxItem;
}

bool openBookByIndex(int idx) {
  safeCloseBook();
  if (idx < 0 || idx >= bookCount) return false;

  String path = String(books[idx].path);
  File f = FS.open(path, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }

  bookFile = f;
  currentBookKey  = prefKeyForBook(path);
  currentBookPath = path;

  uint32_t startOffset = findBookContentStart(bookFile);
  knownPages = 1;
  pageOffsets[0] = startOffset;
  eofReached = false;

  pageIndex = prefs.getInt((currentBookKey + "_p").c_str(), 0);
  if (pageIndex < 0) pageIndex = 0;

  // Build Table of Contents
  buildTableOfContents();

  pageTurnsSinceFull = 0;
  resetSaveThrottle();
  syncWakeState(true);
  return true;
}

void drawStatusBar(uint32_t startOffset) {
  if (g_jumpModeActive) {
    u8g2.setFont(PAGE_FONT);
    u8g2.setCursor(MARGIN_X, H - 2);
    u8g2.print("[JUMP] 1x +10% | 2x -10% | Hold Chapters");
    u8g2.setFont(MAIN_FONT);
    return;
  }

  size_t total = bookFile.size();
  if (total == 0) total = 1;

  int pageTextW = 0;
  if (SHOW_PAGE_NUMBER) {
    u8g2.setFont(PAGE_FONT);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", pageIndex + 1);
    pageTextW = u8g2.getUTF8Width(buf);

    int xTxt = W - MARGIN_X - pageTextW;
    int yTxt = H - 1;
    u8g2.setCursor(xTxt, yTxt);
    u8g2.print(buf);

    u8g2.setFont(MAIN_FONT);
  }

  if (SHOW_PROGRESS_BAR) {
    const int padR = (SHOW_PAGE_NUMBER ? (pageTextW + 6) : 0);
    int w = (W - 2 * MARGIN_X) - padR;
    if (w < 40) w = 40;

    int x0 = MARGIN_X;
    int yBarTop = H - 6;
    int yBarMid = yBarTop + 1;
    int yBarBot = yBarTop + 2;

    int filled = (int)((startOffset * (uint32_t)w) / (uint32_t)total);
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;

    for (int x = x0; x < x0 + w; x++) {
      gfx.drawPixel(x, yBarTop, 1);
      gfx.drawPixel(x, yBarBot, 1);
    }
    for (int x = x0; x < x0 + filled; x++) {
      gfx.drawPixel(x, yBarMid, 1);
    }
  }
}

// ============================================================================
//  Reader rendering
// ============================================================================
void renderCurrentPage() {
  if (!bookFile && !reopenCurrentBookIfNeeded()) {
    drawCenter("Open failed", "Returning to Home");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  if (!bookFile || bookFile.isDirectory()) {
    drawCenter("Open failed", "Returning to Home");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  size_t bookSize = bookFile.size();
  if (bookSize == 0) {
    drawCenter("Book empty", "Returning to Home");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  ensureOffsetsUpTo(pageIndex);
  if (knownPages <= 0) {
    drawCenter("Book empty", "Returning to Home");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  if (pageIndex < 0) pageIndex = 0;
  if (pageIndex >= knownPages) pageIndex = knownPages - 1;

  if (pageOffsets[pageIndex] >= bookSize) {
    pageIndex = 0;
    knownPages = 1;
    pageOffsets[0] = 0;
    eofReached = false;
  }

  uint32_t start = pageOffsets[pageIndex];
  lastPageStartOffset = start;
  bookFile.seek(start);

  bool doFull = (pageTurnsSinceFull >= FULL_REFRESH_EVERY_N_PAGES);
  if (doFull) {
    display.fastmodeOff();
    display.clear();
    pageTurnsSinceFull = 0;
  } else {
    display.fastmodeOn();
  }

  beginPageCanvas();
  u8g2.setFont(MAIN_FONT);

  uint32_t nextOff = readPageFromFile(bookFile, start, true, nullptr);

  const int PREFETCH_AHEAD = 1;
  if (!eofReached && knownPages < MAX_PAGES) {
    if (pageIndex == knownPages - 1) {
      if (nextOff <= start || nextOff >= bookSize) eofReached = true;
      else pageOffsets[knownPages++] = nextOff;
    }
    while (!eofReached && knownPages < MAX_PAGES && knownPages <= (pageIndex + PREFETCH_AHEAD)) {
      uint32_t s = pageOffsets[knownPages - 1];
      uint32_t n = buildNextOffset(s);
      if (n <= s || n >= bookSize) {
        eofReached = true;
        break;
      }
      pageOffsets[knownPages++] = n;
    }
  }

  drawStatusBar(start);
  drawToastIfActive();
  display.update();
}

// ============================================================================
//  Bookmarks storage
// ============================================================================
static String bmKeyFor(const String& bookKey) {
  return bookKey + "_bm";
}

uint8_t loadBookmarksForKey(const String& bookKey, uint16_t outPages[MAX_BOOKMARKS]) {
  uint8_t buf[1 + MAX_BOOKMARKS * 2] = {0};
  size_t got = prefs.getBytes(bmKeyFor(bookKey).c_str(), buf, sizeof(buf));
  if (got < 1) return 0;

  uint8_t count = buf[0];
  if (count > MAX_BOOKMARKS) count = MAX_BOOKMARKS;

  for (uint8_t i = 0; i < count; i++) {
    uint16_t lo = buf[1 + i * 2 + 0];
    uint16_t hi = buf[1 + i * 2 + 1];
    outPages[i] = (uint16_t)((hi << 8) | lo);
  }
  return count;
}

void saveBookmarksForKey(const String& bookKey, const uint16_t pages[MAX_BOOKMARKS], uint8_t count) {
  if (count > MAX_BOOKMARKS) count = MAX_BOOKMARKS;

  uint8_t buf[1 + MAX_BOOKMARKS * 2] = {0};
  buf[0] = count;
  for (uint8_t i = 0; i < count; i++) {
    buf[1 + i * 2 + 0] = (uint8_t)(pages[i] & 0xFF);
    buf[1 + i * 2 + 1] = (uint8_t)((pages[i] >> 8) & 0xFF);
  }
  prefs.putBytes(bmKeyFor(bookKey).c_str(), buf, sizeof(buf));
}

const char* addBookmarkForCurrentBook() {
  if (currentBookKey.length() == 0) return nullptr;

  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(currentBookKey, pages);

  for (uint8_t i = 0; i < count; i++) {
    if ((int)pages[i] == pageIndex) return "Bookmark exists";
  }

  if (count < MAX_BOOKMARKS) pages[count++] = (uint16_t)pageIndex;
  else {
    for (uint8_t i = 1; i < MAX_BOOKMARKS; i++) pages[i - 1] = pages[i];
    pages[MAX_BOOKMARKS - 1] = (uint16_t)pageIndex;
    count = MAX_BOOKMARKS;
  }

  for (uint8_t i = 0; i < count; i++) {
    for (uint8_t j = i + 1; j < count; j++) {
      if (pages[j] < pages[i]) {
        uint16_t t = pages[i];
        pages[i] = pages[j];
        pages[j] = t;
      }
    }
  }

  saveBookmarksForKey(currentBookKey, pages, count);
  return "Bookmark saved";
}

uint32_t pageOffsetForPage(File &f, int page) {
  if (page < 0) page = 0;
  uint32_t off = 0;
  for (int p = 0; p < page; p++) {
    uint32_t next = buildNextOffsetFor(f, off);
    if (next == off) break;
    off = next;
  }
  return off;
}

// ============================================================================
//  Library / About / Bookmarks UI
// ============================================================================
static const int UI_HEADER_TOP = 6;
static const int UI_HEADER_GAP = 6;
static const int UI_LIST_LEFT = MARGIN_X + 2;
static const int UI_BULLET_X = MARGIN_X + 1;
static const int UI_BULLET_R = 2;
static const int UI_INDENT_SELECTED = 8;

static int drawSectionHeader(const char* title, bool bold = true) {
  u8g2.setFont(bold ? BOLD_FONT : MAIN_FONT);
  int ascent = u8g2.getFontAscent();
  int yTitle = UI_HEADER_TOP + ascent;

  u8g2.setCursor(MARGIN_X, yTitle);
  
  char headerText[64];
  if (strcmp(title, "Library") == 0 || strcmp(title, "Home") == 0) {
    time_t nowTime;
    time(&nowTime);
    struct tm* timeinfo = localtime(&nowTime);
    if (timeinfo->tm_year >= 120) {
      strftime(headerText, sizeof(headerText), "Home   %H:%M   %a, %b %d", timeinfo);
    } else {
      strcpy(headerText, "Home");
    }
  } else {
    strncpy(headerText, title, sizeof(headerText) - 1);
    headerText[sizeof(headerText) - 1] = '\0';
  }
  u8g2.print(headerText);

  // WiFi & Charging Indicators
  int iconX = W - MARGIN_X;
  
  #if HAS_BATTERY
  updateBatteryCached(false);
  int pct = g_batValid ? g_batPctShown : 0;
  const int iconW = 18;
  const int iconH = 9;
  iconX -= (iconW + 2);
  int yIcon = 2;
  
  gfx.drawRect(iconX, yIcon, iconW, iconH, 1);
  gfx.fillRect(iconX + iconW, yIcon + 2, 2, iconH - 4, 1);
  
  int innerW = iconW - 2;
  int fillW = (innerW * pct) / 100;
  if (fillW > 0) gfx.fillRect(iconX + 1, yIcon + 1, fillW, iconH - 2, 1);
  if (g_batLow && pct > 0) {
    gfx.drawLine(iconX + 3, yIcon + 2, iconX + 3, yIcon + iconH - 3, 0);
  }
  
  u8g2.setFont(u8g2_font_6x10_tf);
  char batBuf[8];
  snprintf(batBuf, sizeof(batBuf), "%d%%", pct);
  int wTxt = u8g2.getUTF8Width(batBuf);
  iconX -= (wTxt + 4);
  u8g2.setCursor(iconX, yIcon + 8);
  u8g2.print(batBuf);
  #endif

  // Charging symbol +
  bool charging = isUsbConnected();
  if (charging) {
    iconX -= 10;
    u8g2.setFont(MAIN_FONT);
    u8g2.setCursor(iconX, yTitle);
    u8g2.print("+");
  }

  // WiFi symbol [W]
  if (WiFi.status() == WL_CONNECTED) {
    iconX -= 14;
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.setCursor(iconX, yTitle - 1);
    u8g2.print("[W]");
  }

  u8g2.setFont(MAIN_FONT);
  int lineY = yTitle + 4;
  gfx.drawFastHLine(MARGIN_X, lineY, W - (MARGIN_X * 2), 1);

  return lineY + UI_HEADER_GAP + 11;
}

static int drawSectionHeaderWithSubline(const char* title, const String& subline, bool bold = true) {
  int y = drawSectionHeader(title, bold);
  if (subline.length() > 0) {
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(subline.c_str());
    u8g2.setFont(MAIN_FONT);
    y += 11;
  }
  return y;
}

static void drawMenuBulletRow(int yBaseline, const String& label, bool selected, bool boldText = false) {
  int bulletCy = yBaseline - 4;
  if (selected) {
    gfx.fillCircle(UI_BULLET_X + UI_BULLET_R, bulletCy, UI_BULLET_R, 1);
  }

  int textX = UI_LIST_LEFT + (selected ? UI_INDENT_SELECTED : 0);
  u8g2.setFont(boldText ? BOLD_FONT : MAIN_FONT);
  u8g2.setCursor(textX, yBaseline);
  u8g2.print(label.c_str());
  u8g2.setFont(MAIN_FONT);
}

void drawLibrary() {
  prepareMenuFrame();
  buildLibraryEntries();

  u8g2.setFont(MAIN_FONT);
  int ascent  = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH   = (ascent - descent) + LINE_GAP + 1;

  int y = drawSectionHeader("Home", true);
  int totalItems = libraryEntryCount;

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 3) visible = 3;
  if (visible > 6) visible = 6;

  int top = selectedItem - (visible / 2);
  if (top < 0) top = 0;
  if (top > totalItems - visible) top = max(0, totalItems - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= totalItems) break;

    String label = libraryEntryLabel(idx);
    if (libraryEntryTypes[idx] == LIB_ENTRY_FOLDER) {
      label = String("[") + label + "]";
    }

    bool boldText = (libraryEntryTypes[idx] == LIB_ENTRY_BOOK) || (idx == selectedItem);
    drawMenuBulletRow(y, label, idx == selectedItem, boldText);
    y += lineH;
  }

  display.update();
}

void drawAbout() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int lineH = (ascent - u8g2.getFontDescent()) + LINE_GAP + 1;
  int y = drawSectionHeader("About (v" FW_VERSION ")", true);

  String rows[5];
  rows[0] = "1x next / down";
  rows[1] = "2x open / confirm";
  rows[2] = "3x back (universal)";
  rows[3] = "Hold bookmark / cycle back";
  rows[4] = "TXT reader over Wi-Fi";

  for (int i = 0; i < 5; i++) {
    u8g2.setFont(MAIN_FONT);
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(rows[i].c_str());
    y += lineH;
  }

  display.update();
}

void drawBookmarksBookSelect() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent  = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH   = (ascent - descent) + LINE_GAP + 1;

  int y = drawSectionHeader("Bookmarks", true);

  if (bookCount == 0) {
    drawMenuBulletRow(y, "No books", true, false);
    display.update();
    return;
  }

  if (bmBookIndex < 0) bmBookIndex = 0;
  if (bmBookIndex >= bookCount) bmBookIndex = bookCount - 1;

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 2) visible = 2;
  if (visible > 6) visible = 6;

  int top = bmBookIndex - (visible / 2);
  if (top < 0) top = 0;
  if (top > bookCount - visible) top = max(0, bookCount - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= bookCount) break;
    drawMenuBulletRow(y, String(books[idx].name), idx == bmBookIndex, idx == bmBookIndex);
    y += lineH;
  }

  display.update();
}

void drawBookmarksList() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP;

  int y = drawSectionHeader("Bookmarks", true);

  String key = prefKeyForBook(String(books[bmBookIndex].path));
  bmCount = loadBookmarksForKey(key, bmPages);
  if (bmSelIndex >= (int)bmCount) bmSelIndex = max(0, (int)bmCount - 1);

  if (bmCount == 0) {
    drawMenuBulletRow(y, "No bookmarks", true, false);
    display.update();
    return;
  }

  File f = FS.open(String(books[bmBookIndex].path), "r");
  if (!f) {
    drawMenuBulletRow(y, "Open failed", true, false);
    display.update();
    return;
  }

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 1) visible = 1;
  if (visible > 5) visible = 5;

  int top = bmSelIndex - (visible / 2);
  if (top < 0) top = 0;
  if (top > (int)bmCount - visible) top = max(0, (int)bmCount - visible);

  uint32_t cursorOff = 0;
  int cursorPage = 0;
  int firstTargetPage = (int)bmPages[top];
  if (firstTargetPage < 0) firstTargetPage = 0;

  while (cursorPage < firstTargetPage) {
    uint32_t next = buildNextOffsetFor(f, cursorOff);
    if (next == cursorOff) break;
    cursorOff = next;
    cursorPage++;
  }

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= (int)bmCount) break;

    int targetPage = (int)bmPages[idx];
    if (targetPage < 0) targetPage = 0;

    while (cursorPage < targetPage) {
      uint32_t next = buildNextOffsetFor(f, cursorOff);
      if (next == cursorOff) break;
      cursorOff = next;
      cursorPage++;
    }

    String sn = readBookmarkLabelAtOffset(f, cursorOff, targetPage);
    drawMenuBulletRow(y, sn, idx == bmSelIndex, idx == bmSelIndex);
    y += lineH;
  }

  f.close();
  display.update();
}

// ============================================================================
//  Web UI (SPA & JSON API)
// ============================================================================
#include "web_spa.h"

// ---------------------- Bluetooth BLE Upload ----------------------
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

BLEServer* pServer = NULL;
BLEService* pService = NULL;
BLECharacteristic* pControlChar = NULL;
BLECharacteristic* pDataChar = NULL;
bool deviceConnected = false;

File bleFile;
String bleFilePath = "";
size_t bleExpectedSize = 0;
size_t bleWrittenSize = 0;
bool bleTransferActive = false;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    if (bleTransferActive) {
      if (bleFile) bleFile.close();
      if (bleFilePath.length() > 0 && FS.exists(bleFilePath)) {
        FS.remove(bleFilePath);
      }
      bleTransferActive = false;
    }
    BLEDevice::startAdvertising();
  }
};

class ControlCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.print("BLE Control RX: ");
      Serial.println(rxValue);

      if (rxValue.startsWith("START:")) {
        int firstColon = rxValue.indexOf(':');
        int secondColon = rxValue.indexOf(':', firstColon + 1);
        if (firstColon != -1 && secondColon != -1) {
          String filename = rxValue.substring(firstColon + 1, secondColon);
          size_t size = rxValue.substring(secondColon + 1).toInt();

          if (bleTransferActive && bleFile) {
            bleFile.close();
          }

          loadBooks();
          if (bookCount >= MAX_BOOKS) {
            pControlChar->setValue("FAIL:Library full");
            pControlChar->notify();
            return;
          }

          size_t freeBytes = FS.totalBytes() - FS.usedBytes();
          if (freeBytes < size + 8192) {
            pControlChar->setValue("FAIL:Not enough space");
            pControlChar->notify();
            return;
          }

          String clean = sanitizeUploadedFilename(filename);
          bleFilePath = "/reading/" + clean;
          if (FS.exists(bleFilePath)) FS.remove(bleFilePath);

          bleFile = FS.open(bleFilePath, "w");
          if (!bleFile) {
            pControlChar->setValue("FAIL:Cannot open file");
            pControlChar->notify();
            return;
          }

          bleExpectedSize = size;
          bleWrittenSize = 0;
          bleTransferActive = true;

          pControlChar->setValue("OK");
          pControlChar->notify();
          Serial.println("BLE Transfer started: " + bleFilePath + " size " + String(size));
        } else {
          pControlChar->setValue("FAIL:Invalid START command");
          pControlChar->notify();
        }
      }
      else if (rxValue == "END") {
        if (bleTransferActive) {
          if (bleFile) bleFile.close();
          bleTransferActive = false;
          pControlChar->setValue("DONE");
          pControlChar->notify();
          Serial.println("BLE Transfer complete.");
          loadBooks();
        } else {
          pControlChar->setValue("FAIL:No active transfer");
          pControlChar->notify();
        }
      }
      else if (rxValue == "ABORT") {
        if (bleTransferActive) {
          if (bleFile) bleFile.close();
          if (bleFilePath.length() > 0 && FS.exists(bleFilePath)) {
            FS.remove(bleFilePath);
          }
          bleTransferActive = false;
          pControlChar->setValue("ABORTED");
          pControlChar->notify();
          Serial.println("BLE Transfer aborted.");
        }
      }
    }
  }
};

class DataCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    if (!bleTransferActive || !bleFile) return;

    String value = pCharacteristic->getValue();
    uint8_t* data = (uint8_t*)value.c_str();
    size_t len = value.length();

    if (len > 0) {
      bleFile.write(data, len);
      bleWrittenSize += len;
    }
  }
};

// ============================================================================
//  Upload mode start/stop
// ============================================================================
void startUploadServicesOnly() {
  if (g_servicesActive) return;
  g_servicesActive = true;
  Serial.println("Starting upload services (WiFi + WebServer + BLE)...");

  bool connected = false;
  if (g_wifiSsid.length() > 0) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());

    unsigned long startConn = millis();
    while (millis() - startConn < 12000) {
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(200);
    }
  }

  if (!connected) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
  }

  if (MDNS.begin("pala")) {
    MDNS.addService("http", "tcp", 80);
  }

  WiFi.setSleep(false);
  server.begin();

  BLEDevice::init("Pala Reader");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  pService = pServer->createService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

  pControlChar = pService->createCharacteristic(
                   "beb5483e-36e1-4688-b7f5-ea07361b26a8",
                   BLECharacteristic::PROPERTY_READ   |
                   BLECharacteristic::PROPERTY_WRITE  |
                   BLECharacteristic::PROPERTY_NOTIFY
                 );
  pControlChar->addDescriptor(new BLE2902());
  pControlChar->setCallbacks(new ControlCallbacks());

  pDataChar = pService->createCharacteristic(
                "cba1d00f-13d8-4f5b-9fca-dc5c9d1a3c7f",
                BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_WRITE_NR
              );
  pDataChar->setCallbacks(new DataCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("BLE advertising started.");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/books", HTTP_GET, handleApiBooks);
  server.on("/api/wifi", HTTP_GET, handleApiWifi);
  server.on("/api/settings", HTTP_GET, handleApiSettings);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/set-page", HTTP_GET, handleSetPage);
  server.on("/move", HTTP_POST, handleMoveBook);
  server.on("/save-book", HTTP_POST, handleSaveBook);
  server.on("/del", HTTP_GET, handleDelete);
  server.on("/mkdir", HTTP_POST, handleCreateFolder);
  server.on("/rmdir", HTTP_GET, handleDeleteFolder);
  server.on("/add-wifi", HTTP_POST, handleAddWifi);
  server.on("/del-wifi", HTTP_GET, handleDelWifi);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/reset", HTTP_GET, handleResetConfirm);
  server.on("/reset", HTTP_POST, handleResetDo);
  server.on("/del-sleep", HTTP_GET, handleDeleteSleepImg);

  server.on("/upload-sleep", HTTP_POST, handleUploadSleepDone, []() {
    HTTPUpload& upS = server.upload();

    if (upS.status == UPLOAD_FILE_START) {
      g_sleepUploadOk = false;
      g_sleepUploadError = "";
      g_sleepUploadTmpPath = "/sleep.bin.tmp";
      if (FS.exists(g_sleepUploadTmpPath)) FS.remove(g_sleepUploadTmpPath);
      sleepUploadFile = FS.open(g_sleepUploadTmpPath, "w");
      if (!sleepUploadFile) g_sleepUploadError = "Cannot create temp sleep file";
    }
    else if (upS.status == UPLOAD_FILE_WRITE) {
      if (sleepUploadFile) sleepUploadFile.write(upS.buf, upS.currentSize);
    }
    else if (upS.status == UPLOAD_FILE_END) {
      if (sleepUploadFile) sleepUploadFile.close();
      File f = FS.open(g_sleepUploadTmpPath, "r");
      size_t sz = f ? f.size() : 0;
      if (f) f.close();

      if (sz != 3904) {
        if (FS.exists(g_sleepUploadTmpPath)) FS.remove(g_sleepUploadTmpPath);
        g_sleepUploadError = "Sleep image must be exactly 3904 bytes";
        g_sleepUploadOk = false;
      } else {
        if (FS.exists("/sleep.bin")) FS.remove("/sleep.bin");
        if (FS.rename(g_sleepUploadTmpPath, "/sleep.bin")) {
          g_sleepUploadOk = true;
        } else {
          if (FS.exists(g_sleepUploadTmpPath)) FS.remove(g_sleepUploadTmpPath);
          g_sleepUploadError = "Failed to save sleep image";
        }
      }
      g_sleepUploadTmpPath = "";
    }
    else if (upS.status == UPLOAD_FILE_ABORTED) {
      if (sleepUploadFile) sleepUploadFile.close();
      if (g_sleepUploadTmpPath.length() > 0 && FS.exists(g_sleepUploadTmpPath)) FS.remove(g_sleepUploadTmpPath);
      g_sleepUploadError = "Sleep image upload aborted";
      g_sleepUploadOk = false;
      g_sleepUploadTmpPath = "";
    }
  });

  server.on("/upload", HTTP_POST, handleUploadDone, []() {
    HTTPUpload& up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
      g_uploadOk = false;
      g_uploadError = "";
      g_uploadFinalName = "";
      uploadPending = "";
      uploadPath = "";

      loadBooks();
      if (bookCount >= MAX_BOOKS) {
        g_uploadError = "Library full";
        return;
      }

      size_t freeBytes = FS.totalBytes() - FS.usedBytes();
      if (freeBytes < 8192) {
        g_uploadError = "Not enough free space";
        return;
      }

      String folder = "";
      if (server.hasArg("folder")) {
        folder = sanitizeFolderInput(server.arg("folder"));
      }
      String destDir = "/reading";
      if (folder.length() > 0) {
        destDir = "/reading/" + folder;
        ensureDirRecursive(destDir);
      }
      String clean = sanitizeUploadedFilename(up.filename);
      g_uploadFinalName = clean;
      String finalPath = destDir + "/" + clean;
      uploadPath = finalPath + ".tmp";

      if (FS.exists(uploadPath)) FS.remove(uploadPath);
      uploadFile = FS.open(uploadPath, "w");
      if (!uploadFile) {
        g_uploadError = "Cannot create temp upload file";
        uploadPath = "";
      }
    }
    else if (up.status == UPLOAD_FILE_WRITE) {
      if (uploadFile && up.currentSize > 0) {
        String chunk = uploadPending + String((const char*)up.buf, up.currentSize);
        int len = (int)chunk.length();
        if (len > 3) {
          uploadPending = chunk.substring(len - 3);
          chunk = chunk.substring(0, len - 3);
        } else {
          uploadPending = chunk;
          chunk = "";
        }

        if (chunk.length() > 0) {
          uploadFile.print(normalizeTypography(chunk));
        }
      }
    }
    else if (up.status == UPLOAD_FILE_END) {
      if (uploadFile) {
        if (uploadPending.length() > 0) {
          uploadFile.print(normalizeTypography(uploadPending));
          uploadPending = "";
        }
        uploadFile.close();

        if (uploadPath.length() > 0 && up.totalSize > 0) {
          String finalPath = uploadPath.substring(0, uploadPath.length() - 4);
          if (FS.exists(finalPath)) FS.remove(finalPath);
          if (FS.rename(uploadPath, finalPath)) {
            g_uploadOk = true;
          } else {
            if (FS.exists(uploadPath)) FS.remove(uploadPath);
            g_uploadError = "Failed to finalize upload";
          }
        } else {
          if (uploadPath.length() > 0 && FS.exists(uploadPath)) FS.remove(uploadPath);
          g_uploadError = "Empty upload";
        }
        uploadPath = "";
      } else {
        if (uploadPath.length() > 0 && FS.exists(uploadPath)) FS.remove(uploadPath);
        if (g_uploadError.length() == 0) g_uploadError = "Upload failed";
        uploadPath = "";
      }
    }
    else if (up.status == UPLOAD_FILE_ABORTED) {
      if (uploadFile) uploadFile.close();
      if (uploadPath.length() > 0 && FS.exists(uploadPath)) FS.remove(uploadPath);
      uploadPending = "";
      uploadPath = "";
      g_uploadOk = false;
      g_uploadError = "Upload aborted";
    }
  });
}

void stopUploadServicesOnly() {
  if (!g_servicesActive) return;
  g_servicesActive = false;
  Serial.println("Stopping upload services...");

  server.stop();

  if (uploadFile) uploadFile.close();
  if (sleepUploadFile) sleepUploadFile.close();

  MDNS.end();

  BLEDevice::deinit(true);
  pServer = NULL;
  pService = NULL;
  pControlChar = NULL;
  pDataChar = NULL;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();

  uploadPending = "";
  uploadPath = "";
  g_uploadOk = false;
  g_uploadError = "";
  g_uploadFinalName = "";

  g_sleepUploadOk = false;
  g_sleepUploadError = "";
  g_sleepUploadTmpPath = "";
}

bool syncWithCloud() {
  if (g_wifiSsid.length() == 0) return false;

  prepareMenuFrame();

  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Cloud Sync");
  gfx.drawFastHLine(MARGIN_X, 16, W - (MARGIN_X * 2), 1);
  u8g2.setFont(MAIN_FONT);
  u8g2.setCursor(MARGIN_X, 33);
  u8g2.print("Connecting to WiFi...");
  display.update();

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());

  bool connected = false;
  unsigned long startConn = millis();
  while (millis() - startConn < 15000) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(200);
  }

  if (!connected) {
    WiFi.disconnect(true, true);
    return false;
  }

  u8g2.setCursor(MARGIN_X, 48);
  u8g2.print("Syncing with server...");
  display.update();

  HTTPClient http;
  String mac = WiFi.macAddress();
  
  // Register & Pairing
  while (true) {
    http.begin("http://pala.felixresch.com/api/device/register");
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST("{\"mac_address\":\"" + mac + "\"}");
    if (httpCode == 200) {
      String payload = http.getString();
      ALLOC_JSON_DOC(regDoc, 512);
      DeserializationError err = deserializeJson(regDoc, payload);
      if (!err && regDoc["status"] == "pairing") {
        String code = regDoc["code"].as<String>();
        prepareMenuFrame();
        u8g2.setFont(BOLD_FONT);
        u8g2.setCursor(MARGIN_X, 20);
        u8g2.print("Device Pairing");
        u8g2.setFont(MAIN_FONT);
        u8g2.setCursor(MARGIN_X, 40);
        u8g2.print("Go to Pala Cloud App");
        u8g2.setCursor(MARGIN_X, 55);
        u8g2.print("Enter code:");
        u8g2.setFont(BOLD_FONT);
        u8g2.setCursor(MARGIN_X, 75);
        u8g2.print(code.c_str());
        u8g2.setFont(MAIN_FONT);
        u8g2.setCursor(MARGIN_X, 100);
        u8g2.print("Click button to skip");
        display.update();

        unsigned long waitStart = millis();
        bool skipped = false;
        while (millis() - waitStart < 5000) {
           if (digitalRead(BTN1) == LOW) { skipped = true; break; }
           delay(50);
        }
        if (skipped) { http.end(); WiFi.disconnect(true, true); return false; }
        http.end();
        continue;
      } else if (!err && regDoc["status"] == "ok") {
        http.end();
        break; // Registered!
      }
    }
    http.end();
    break; // Proceed or fail gracefully
  }

  prepareMenuFrame();
  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Cloud Sync");
  gfx.drawFastHLine(MARGIN_X, 16, W - (MARGIN_X * 2), 1);
  u8g2.setFont(MAIN_FONT);
  u8g2.setCursor(MARGIN_X, 33);
  u8g2.print("Syncing with server...");
  display.update();
  
  // Pull
  http.begin("http://pala.felixresch.com/api/sync/pull?mac=" + mac);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
ALLOC_JSON_DOC(doc, 1024);
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      if (doc.containsKey("font_size")) {
        int new_sz = doc["font_size"];
        applyFontSize(new_sz);
        prefs.putInt("cfg_font", new_sz);
      }
      if (doc.containsKey("sleep_timeout")) {
         int new_slp = doc["sleep_timeout"];
         g_sleepSecs = new_slp;
         prefs.putInt("cfg_sleep", new_slp);
         SLEEP_AFTER_MS = g_sleepSecs * 1000UL;
      }
      if (doc.containsKey("line_gap")) {
         int new_gap = doc["line_gap"];
         LINE_GAP = new_gap;
         prefs.putInt("cfg_lgap", new_gap);
         invalidateMetrics();
      }
      if (doc.containsKey("books")) {
        JsonArray arr = doc["books"].as<JsonArray>();
        for (JsonObject b : arr) {
          int b_id = b["id"];
          String b_title = b["title"].as<String>();
          String safeTitle = sanitizeUploadedFilename(b_title);
          if (!safeTitle.endsWith(".txt")) safeTitle += ".txt";
          String fpath = "/reading/" + safeTitle;
          if (!FS.exists(fpath)) {
             u8g2.setCursor(MARGIN_X, 60);
             u8g2.print("Downloading book...");
             display.update();

             HTTPClient httpDl;
             httpDl.begin("http://pala.felixresch.com/api/book/" + String(b_id) + "?mac=" + mac);
             int dlCode = httpDl.GET();
             if (dlCode == 200) {
               File f = FS.open(fpath, "w");
               if (f) {
                 httpDl.writeToStream(&f);
                 f.close();
               }
             }
             httpDl.end();
          }
        }
      }
    }
  }
  http.end();

  // Push
  http.begin("http://pala.felixresch.com/api/sync/push");
  http.addHeader("Content-Type", "application/json");
ALLOC_JSON_DOC(pushDoc, 1024);
  pushDoc["mac_address"] = mac;
if (HAS_BATTERY) {
    pushDoc["battery_level"] = g_batPct;
  } else {
    pushDoc["battery_level"] = 100;
  }
  pushDoc["font_size"] = g_fontSize;
  pushDoc["sleep_timeout"] = g_sleepSecs;
  pushDoc["line_gap"] = LINE_GAP;
  String pushPayload;
  serializeJson(pushDoc, pushPayload);
  http.POST(pushPayload);
  http.end();

  WiFi.disconnect(true, true);
  return true;
}

void startUploadMode() {
  mode = MODE_UPLOAD;
  g_uploadStartMs = millis();

  prepareMenuFrame();
  
  if (syncWithCloud()) {
    stopUploadModeToLibrary();
    return;
  }

  prepareMenuFrame();
  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Upload Mode");
  gfx.drawFastHLine(MARGIN_X, 16, W - (MARGIN_X * 2), 1);
  u8g2.setFont(MAIN_FONT);

  u8g2.setCursor(MARGIN_X, 33);
  u8g2.print("Initializing...");
  display.update();

  startUploadServicesOnly();

  prepareMenuFrame();
  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Upload Mode");
  gfx.drawFastHLine(MARGIN_X, 16, W - (MARGIN_X * 2), 1);
  u8g2.setFont(MAIN_FONT);

  IPAddress ip;
  String url;
  if (WiFi.getMode() == WIFI_MODE_STA) {
    ip = WiFi.localIP();
    url = String("http://") + ip.toString();
    int y = 33;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("Mode: Connected (STA)"); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("SSID: "); u8g2.print(g_wifiSsid.c_str()); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("URL:  "); u8g2.print(url.c_str()); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("Or:   http://pala.local"); y += 16;
  } else {
    ip = WiFi.softAPIP();
    url = String("http://") + ip.toString();
    int y = 33;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("Mode: Access Point (AP)"); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("SSID: "); u8g2.print(AP_SSID); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("Pass: "); u8g2.print(AP_PASS); y += 14;
    u8g2.setCursor(MARGIN_X, y); u8g2.print("URL:  "); u8g2.print(url.c_str()); y += 16;
  }

  u8g2.setCursor(MARGIN_X, 108);
  u8g2.print("BLE: Pala Reader  Click to exit");
  display.update();
}

void stopUploadModeToLibrary() {
  stopUploadServicesOnly();
  loadBooks();
  mode = MODE_LIBRARY;
  resetInputFrontend();
  drawLibrary();
}

void drawSettings() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP + 1;
  int y = drawSectionHeader("Settings", true);

  for (int i = 1; i < SETTINGS_COUNT; i++) { // Skip Back (0)
    String label;
    switch (i) {
      case SET_ENTRY_UPLOAD:
        label = "Upload Mode";
        break;
      case SET_ENTRY_ABOUT:
        label = "About";
        break;
      case SET_ENTRY_NIGHT_MODE:
        label = "Night Mode: " + String(g_nightMode ? "On" : "Off");
        break;
      case SET_ENTRY_TEXT_SIZE:
        label = "Text Size: " + String(g_fontSize) + "px";
        break;
      case SET_ENTRY_SCREENSAVER: {
        String modeStr = "Picture";
        if (g_screensaverMode == 1) modeStr = "Spotify";
        else if (g_screensaverMode == 2) modeStr = "Daily Board";
        else if (g_screensaverMode == 3) modeStr = "Clock/Cal";
        label = "Screensaver: " + modeStr;
        break;
      }
      case SET_ENTRY_MEMORY: {
        size_t total = FS.totalBytes();
        size_t used = FS.usedBytes();
        int pct = (total > 0) ? (used * 100 / total) : 0;
        size_t booksSize = getFolderSize("/reading");
        size_t freeSize = total - used;
        label = "Storage: " + String(pct) + "% (" + String(booksSize / 1024) + "K books, " + String(freeSize / 1024) + "K free)";
        break;
      }
    }
    drawMenuBulletRow(y, label, i == selectedSettingItem, i == selectedSettingItem);
    y += lineH;
  }
  display.update();
}

void handleModeSettings() {
  if (btns.b1.shortClick) {
    selectedSettingItem++;
    if (selectedSettingItem >= SETTINGS_COUNT) selectedSettingItem = 1;
    drawSettings();
    return;
  }

  if (btns.b2.shortClick) {
    selectedSettingItem--;
    if (selectedSettingItem < 1) selectedSettingItem = SETTINGS_COUNT - 1;
    drawSettings();
    return;
  }

  if (btns.b2.doubleClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
    return;
  }

  if (btns.b1.doubleClick) {
    switch (selectedSettingItem) {
      case SET_ENTRY_UPLOAD:
        startUploadMode();
        break;
      case SET_ENTRY_ABOUT:
        mode = MODE_ABOUT;
        drawAbout();
        break;
      case SET_ENTRY_NIGHT_MODE:
        g_nightMode = !g_nightMode;
        prefs.putBool("cfg_invert", g_nightMode);
        drawSettings();
        break;
      case SET_ENTRY_TEXT_SIZE: {
        int nextSz = 8;
        if (g_fontSize == 8) nextSz = 10;
        else if (g_fontSize == 10) nextSz = 12;
        else if (g_fontSize == 12) nextSz = 14;
        else nextSz = 8;
        applyFontSize(nextSz);
        prefs.putInt("cfg_font", g_fontSize);
        drawSettings();
        break;
      }
      case SET_ENTRY_SCREENSAVER:
        g_screensaverMode = (g_screensaverMode + 1) % 4;
        prefs.putInt("cfg_scr_mode", g_screensaverMode);
        g_spotifyScreensaver = (g_screensaverMode == 1);
        prefs.putBool("spot_scr", g_spotifyScreensaver);
        drawSettings();
        break;
      case SET_ENTRY_MEMORY:
        // Double clicking storage redraws to refresh details
        drawSettings();
        break;
    }
    return;
  }
}

void drawSleepScreen() {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();

  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }

  File sf = FS.open("/sleep.bin", "r");
  if (sf && sf.size() >= 3904) {
    static uint8_t sleepBuf[3904];
    sf.read(sleepBuf, 3904);
    sf.close();
    gfx.fillScreen(1);
    gfx.drawXBitmap(0, 0, sleepBuf, W, H, 0);
  } else {
    if (sf) sf.close();
    gfx.fillScreen(1);
    gfx.drawXBitmap(0, 0, pala_one_sleep_black_icon_v4_bits, W, H, 0);
  }

  // Draw Standby Widget
  gfx.drawFastHLine(MARGIN_X, H - 15, W - (MARGIN_X * 2), 1);
  u8g2.setFont(u8g2_font_5x8_tf);
  
  #if HAS_BATTERY
  updateBatteryCached(true);
  char batBuf[12];
  snprintf(batBuf, sizeof(batBuf), "%d%%", g_batValid ? g_batPctShown : 0);
  int batW = u8g2.getUTF8Width(batBuf);
  u8g2.setCursor(W - MARGIN_X - batW, H - 4);
  u8g2.print(batBuf);
  #endif

  String wakePath = prefs.getString("wake_path", "");
  if (wakePath.length() > 0) {
    String bookName = stripTxtExt(lastPathComponent(wakePath));
    bookName.replace('_', ' ');
    String key = prefKeyForBook(wakePath);
    int pct = prefs.getInt((key + "_pct").c_str(), -1);
    String infoStr = bookName;
    if (pct >= 0) infoStr += " (" + String(pct) + "%)";
    int maxW = W - (MARGIN_X * 2) - 40;
    if (u8g2.getUTF8Width(infoStr.c_str()) > maxW) {
      while (infoStr.length() > 0 && u8g2.getUTF8Width((infoStr + "...").c_str()) > maxW) {
        infoStr.remove(infoStr.length() - 1);
      }
      infoStr += "...";
    }
    u8g2.setCursor(MARGIN_X, H - 4);
    u8g2.print(infoStr.c_str());
  } else {
    u8g2.setCursor(MARGIN_X, H - 4);
    u8g2.print("Pala E-Reader - Standby");
  }

  u8g2.setFont(MAIN_FONT);
  display.update();
}

// ============================================================================
//  Deep sleep
// ============================================================================
void goToSleep() {
  if (!ENABLE_DEEP_SLEEP) return;

  if (bmPreviewActive) {
    int tmpPage = pageIndex;
    pageIndex = bmPreviewSavedPage;
    saveProgressThrottled(true);
    pageIndex = tmpPage;
  } else if (mode == MODE_READER) {
    saveProgressThrottled(true);
  }

  delay(50);

  bool wasReading = (mode == MODE_READER || mode == MODE_BM_PREVIEW) &&
                    currentBookPath.length() > 0;
  syncWakeState(wasReading);

  safeCloseBook();

  // Screensavers: 0=Pic, 1=Spot, 2=Chess, 3=Clock
  if (g_screensaverMode == 1 && g_spotifyClientId.length() > 0 && g_spotifyRefreshToken.length() > 0) {
    rtc_inSpotifyScreensaver = true;
    rtc_inClockScreensaver = false;
    if (ensureSpotifyWiFi()) {
      SpotifyTrackInfo track = getSpotifyCurrentlyPlaying();
      drawSpotifyScreen(track);
    } else {
      drawSleepScreen();
    }
  } else if (g_screensaverMode == 2) {
    rtc_inSpotifyScreensaver = false;
    if (rtc_chessPuzzleSolvedToday) {
      rtc_inClockScreensaver = true;
      drawClockScreen();
    } else {
      rtc_inClockScreensaver = false;
      drawDailyBoardScreensaver();
    }
  } else if (g_screensaverMode == 3) {
    rtc_inSpotifyScreensaver = false;
    rtc_inClockScreensaver = true;
    drawClockScreen();
  } else {
    rtc_inSpotifyScreensaver = false;
    rtc_inClockScreensaver = false;
    drawSleepScreen();
  }

  delay(600);

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();
  btStop();

  Platform::prepareToSleep();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN1, 0);

  if (rtc_inSpotifyScreensaver) {
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  } else if (rtc_inClockScreensaver) {
    esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL); // 10 minutes clock updates
  }

  delay(50);
  esp_deep_sleep_start();
}

static inline void idleLightSleepMaybe() {
  if (mode == MODE_UPLOAD) return;
  if (!ENABLE_DEEP_SLEEP) return;
  if (toastUntilMs != 0 && (int32_t)(millis() - toastUntilMs) <= 0) return;
  if (btnQTail != btnQHead) return;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN1, 0);
  esp_light_sleep_start();

  while (digitalRead(BTN1) == LOW) delay(5);
  delay(DEBOUNCE_MS + 5);
}

// ============================================================================
//  Chess Application Functions
// ============================================================================

void parseFEN(const String& fen, char board[8][8], bool& whiteToMove) {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      board[r][c] = '.';
    }
  }

  int spaceIdx = fen.indexOf(' ');
  String placement = (spaceIdx == -1) ? fen : fen.substring(0, spaceIdx);

  int r = 0, c = 0;
  for (int i = 0; i < placement.length(); i++) {
    char ch = placement[i];
    if (ch == '/') {
      r++;
      c = 0;
    } else if (ch >= '1' && ch <= '8') {
      c += (ch - '0');
    } else {
      if (r < 8 && c < 8) {
        board[r][c] = ch;
        c++;
      }
    }
  }

  whiteToMove = true;
  if (spaceIdx != -1 && spaceIdx + 1 < fen.length()) {
    whiteToMove = (fen[spaceIdx + 1] == 'w');
  }
}

void uciToCoords(const String& uci, int& r1, int& c1, int& r2, int& c2) {
  c1 = uci[0] - 'a';
  r1 = '8' - uci[1];
  c2 = uci[2] - 'a';
  r2 = '8' - uci[3];
}

bool onBoard(int r, int c) {
  return r >= 0 && r < 8 && c >= 0 && c < 8;
}

bool isWhitePiece(char p) {
  return p >= 'A' && p <= 'Z';
}

bool isBlackPiece(char p) {
  return p >= 'a' && p <= 'z';
}

bool sameColor(char p1, char p2) {
  if (p1 == '.' || p2 == '.') return false;
  return (isWhitePiece(p1) && isWhitePiece(p2)) || (isBlackPiece(p1) && isBlackPiece(p2));
}

int getPseudoMoves(char board[8][8], int r, int c, ChessMove moves[32]) {
  char p = board[r][c];
  if (p == '.') return 0;

  bool white = isWhitePiece(p);
  int count = 0;
  char lowerP = white ? (p + 32) : p;

  if (lowerP == 'p') {
    int dir = white ? -1 : 1;
    int startRank = white ? 6 : 1;

    int nr = r + dir;
    if (onBoard(nr, c) && board[nr][c] == '.') {
      moves[count++] = {r, c, nr, c};
      int nnr = r + 2 * dir;
      if (r == startRank && onBoard(nnr, c) && board[nnr][c] == '.') {
        moves[count++] = {r, c, nnr, c};
      }
    }

    int dc[] = {-1, 1};
    for (int i = 0; i < 2; i++) {
      int nc = c + dc[i];
      if (onBoard(nr, nc) && board[nr][nc] != '.' && !sameColor(p, board[nr][nc])) {
        moves[count++] = {r, c, nr, nc};
      }
    }
  } else if (lowerP == 'n') {
    int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
      int nr = r + dr[i];
      int nc = c + dc[i];
      if (onBoard(nr, nc) && (board[nr][nc] == '.' || !sameColor(p, board[nr][nc]))) {
        moves[count++] = {r, c, nr, nc};
      }
    }
  } else if (lowerP == 'b' || lowerP == 'q') {
    int dr[] = {-1, -1, 1, 1};
    int dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; d++) {
      for (int step = 1; step < 8; step++) {
        int nr = r + dr[d] * step;
        int nc = c + dc[d] * step;
        if (!onBoard(nr, nc)) break;
        if (board[nr][nc] == '.') {
          moves[count++] = {r, c, nr, nc};
        } else {
          if (!sameColor(p, board[nr][nc])) {
            moves[count++] = {r, c, nr, nc};
          }
          break;
        }
      }
    }
  }
  
  if (lowerP == 'r' || lowerP == 'q') {
    int dr[] = {-1, 1, 0, 0};
    int dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
      for (int step = 1; step < 8; step++) {
        int nr = r + dr[d] * step;
        int nc = c + dc[d] * step;
        if (!onBoard(nr, nc)) break;
        if (board[nr][nc] == '.') {
          moves[count++] = {r, c, nr, nc};
        } else {
          if (!sameColor(p, board[nr][nc])) {
            moves[count++] = {r, c, nr, nc};
          }
          break;
        }
      }
    }
  } else if (lowerP == 'k') {
    int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
      int nr = r + dr[i];
      int nc = c + dc[i];
      if (onBoard(nr, nc) && (board[nr][nc] == '.' || !sameColor(p, board[nr][nc]))) {
        moves[count++] = {r, c, nr, nc};
      }
    }
  }

  return count;
}

int getAvailablePieces(char board[8][8], bool playerIsWhite, ChessPiecePos pieces[16]) {
  int count = 0;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      char p = board[r][c];
      if (p == '.') continue;
      bool isWhite = isWhitePiece(p);
      if (isWhite == playerIsWhite) {
        ChessMove tempMoves[32];
        if (getPseudoMoves(board, r, c, tempMoves) > 0) {
          char lowerP = isWhite ? (p + 32) : p;
          pieces[count++] = {r, c, lowerP};
        }
      }
    }
  }

  auto getRelevance = [](char type) {
    switch (type) {
      case 'q': return 0;
      case 'r': return 1;
      case 'b': return 2;
      case 'n': return 3;
      case 'p': return 4;
      case 'k': return 5;
      default: return 6;
    }
  };

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (getRelevance(pieces[j].type) < getRelevance(pieces[i].type)) {
        ChessPiecePos tmp = pieces[i];
        pieces[i] = pieces[j];
        pieces[j] = tmp;
      }
    }
  }

  return count;
}

bool isPuzzleCompleted(const String& id) {
  File f = LittleFS.open("/chess_done.txt", "r");
  if (!f) return false;
  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line == id) { found = true; break; }
  }
  f.close();
  return found;
}

void markPuzzleCompleted(const String& id) {
  if (id.length() == 0) return;
  File f = LittleFS.open("/chess_done.txt", "a");
  if (f) {
    f.println(id);
    f.close();
  }
}

bool fetchChessPuzzle() {
  if (WiFi.status() != WL_CONNECTED) {
    drawCenter("Chess", "Connecting to Wi-Fi...");
    if (!connectSTAWithMulti()) {
      drawCenter("Chess", "Wi-Fi connection failed!");
      delay(1500);
      return false;
    }
  }

  drawCenter("Chess", "Fetching puzzle...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  uint32_t rStart = esp_random() % 100;
  String vercelUrl = "https://chess-puzzles-api.vercel.app/puzzles?min_rating=" + String(g_chessElo - 100) + 
                     "&max_rating=" + String(g_chessElo + 100) + 
                     "&start=" + String(rStart) + 
                     "&limit=5";

  if (!http.begin(client, vercelUrl)) {
    drawCenter("Chess", "API connection failed");
    delay(1500);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    vercelUrl = "https://chess-puzzles-api.vercel.app/puzzles?min_rating=" + String(g_chessElo - 100) + 
                 "&max_rating=" + String(g_chessElo + 100) + 
                 "&limit=5";
    if (http.begin(client, vercelUrl)) {
      httpCode = http.GET();
    }
  }

  String puzzleId = "";
  if (httpCode == 200) {
    String payload = http.getString();
    int idIdx = payload.indexOf("\"PuzzleId\":\"");
    String firstId = "";
    while (idIdx != -1) {
      int start = idIdx + 12;
      int end = payload.indexOf("\"", start);
      if (end != -1) {
        String candId = payload.substring(start, end);
        if (firstId == "") firstId = candId;
        if (!isPuzzleCompleted(candId)) {
          puzzleId = candId;
          break;
        }
      }
      idIdx = payload.indexOf("\"PuzzleId\":\"", end);
    }
    if (puzzleId == "" && firstId != "") {
      puzzleId = firstId;
    }
  }
  http.end();

  if (puzzleId.length() == 0) {
    drawCenter("Chess", "No puzzles found for Elo");
    delay(1500);
    return false;
  }

  String lichessUrl = "https://lichess.org/api/puzzle/" + puzzleId;
  if (!http.begin(client, lichessUrl)) {
    drawCenter("Chess", "Lichess API failed");
    delay(1500);
    return false;
  }

  httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    drawCenter("Chess", "Lichess request failed");
    delay(1500);
    return false;
  }

  String payload = http.getString();
  http.end();

  int fenIdx = payload.indexOf("\"fen\":\"");
  if (fenIdx == -1) return false;
  int fenStart = fenIdx + 7;
  int fenEnd = payload.indexOf("\"", fenStart);
  if (fenEnd == -1) return false;
  String fen = payload.substring(fenStart, fenEnd);

  int ratingIdx = payload.indexOf("\"rating\":");
  if (ratingIdx != -1) {
    int ratingStart = ratingIdx + 9;
    int ratingEnd = ratingStart;
    while (ratingEnd < payload.length() && payload[ratingEnd] >= '0' && payload[ratingEnd] <= '9') {
      ratingEnd++;
    }
    g_chessPuzzleRating = payload.substring(ratingStart, ratingEnd).toInt();
  } else {
    g_chessPuzzleRating = g_chessElo;
  }

  int solIdx = payload.indexOf("\"solution\":[");
  if (solIdx == -1) return false;
  int solStart = solIdx + 12;
  int solEnd = payload.indexOf("]", solStart);
  if (solEnd == -1) return false;
  String solStr = payload.substring(solStart, solEnd);

  g_chessSolutionCount = 0;
  int parsePtr = 0;
  while (parsePtr < solStr.length() && g_chessSolutionCount < 12) {
    int quoteStart = solStr.indexOf("\"", parsePtr);
    if (quoteStart == -1) break;
    int quoteEnd = solStr.indexOf("\"", quoteStart + 1);
    if (quoteEnd == -1) break;
    g_chessSolution[g_chessSolutionCount++] = solStr.substring(quoteStart + 1, quoteEnd);
    parsePtr = quoteEnd + 1;
  }

  g_chessPuzzleId = puzzleId;
  g_chessCurrentMoveIdx = 0;
  g_chessWrongTries = 0;
  g_chessAlreadyFailed = false;
  
  parseFEN(fen, g_chessBoard, g_chessWhiteToMove);
  g_chessPlayerIsWhite = g_chessWhiteToMove;
  rtc_chessPuzzleSolvedToday = false;

  return true;
}

// Larger Chess Layout
int sqSize = 15;
int boardX = 129;
int boardY = 1;

void drawBoardGrid() {
  gfx.disableInversion = true; // Chess board drawn normally
  for (int i = 0; i <= 8; i++) {
    gfx.drawFastHLine(boardX, boardY + i * sqSize, 8 * sqSize, 1);
    gfx.drawFastVLine(boardX + i * sqSize, boardY, 8 * sqSize, 1);
  }
}

void drawDottedRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  for (int16_t i = 0; i < w; i += 2) {
    gfx.drawPixel(x + i, y, color);
    gfx.drawPixel(x + i, y + h - 1, color);
  }
  for (int16_t i = 0; i < h; i += 2) {
    gfx.drawPixel(x, y + i, color);
    gfx.drawPixel(x + w - 1, y + i, color);
  }
}

void drawPiece(int r, int c, char p, bool isSelected, bool isTarget) {
  int x = boardX + c * sqSize;
  int y = boardY + r * sqSize;
  bool whitePiece = isWhitePiece(p);
  char type = whitePiece ? p : (p - 32);
  bool isDarkSquare = ((r + c) % 2 == 1);

  gfx.disableInversion = true; // Render board pieces normally

  if (!whitePiece) {
    // Black piece: black background
    gfx.fillRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
    u8g2.setFontMode(1);
    u8g2.setForegroundColor(0);
    u8g2.setFont(u8g2_font_helvB10_tf);
    char buf[2] = {type, '\0'};
    int w = u8g2.getUTF8Width(buf);
    u8g2.setCursor(x + (sqSize - w) / 2, y + 12);
    u8g2.print(buf);

    if (isSelected) {
      drawDottedRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 0);
    } else if (isTarget) {
      gfx.drawRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 0);
      gfx.drawRect(x + 2, y + 2, sqSize - 4, sqSize - 4, 0);
    }
  } else {
    // White piece: white background
    gfx.fillRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 0);
    if (isDarkSquare && !isSelected && !isTarget) {
      gfx.drawRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
    }

    u8g2.setFontMode(1);
    u8g2.setForegroundColor(1);
    u8g2.setFont(u8g2_font_helvB10_tf);
    char buf[2] = {type, '\0'};
    int w = u8g2.getUTF8Width(buf);
    u8g2.setCursor(x + (sqSize - w) / 2, y + 12);
    u8g2.print(buf);

    if (isSelected) {
      drawDottedRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
    } else if (isTarget) {
      gfx.drawRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
      gfx.drawRect(x + 2, y + 2, sqSize - 4, sqSize - 4, 1);
    }
  }
}

void drawSquare(int r, int c, bool isSelected, bool isTarget) {
  int x = boardX + c * sqSize;
  int y = boardY + r * sqSize;
  bool isDarkSquare = ((r + c) % 2 == 1);

  gfx.disableInversion = true; // Render normally

  gfx.fillRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 0);

  if (isDarkSquare) {
    if (!isSelected && !isTarget) {
      gfx.drawRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
    }
    gfx.drawPixel(x + 7, y + 7, 1);
  }

  if (isSelected) {
    drawDottedRect(x + 1, y + 1, sqSize - 2, sqSize - 2, 1);
  } else if (isTarget) {
    gfx.fillCircle(x + 7, y + 7, 3, 1);
  }
}

void drawResultIcon(int rx, int y, int status) {
  gfx.drawRect(rx, y, 9, 9, 1);
  if (status == 1) {
    gfx.drawLine(rx + 2, y + 4, rx + 4, y + 6, 1);
    gfx.drawLine(rx + 4, y + 6, rx + 7, y + 2, 1);
  } else if (status == 2) {
    gfx.drawLine(rx + 2, y + 2, rx + 6, y + 6, 1);
    gfx.drawLine(rx + 2, y + 6, rx + 6, y + 2, 1);
  }
}

void drawChessScreen() {
  prepareMenuFrame();
  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }

  drawBoardGrid();

  int selectedR = -1, selectedC = -1;
  if (g_chessSubMode == CHESS_SUB_PIECE_SEL && g_chessAvailablePiecesCount > 0) {
    selectedR = g_chessAvailablePieces[g_chessSelectedPieceIdx].r;
    selectedC = g_chessAvailablePieces[g_chessSelectedPieceIdx].c;
  } else if (g_chessSubMode == CHESS_SUB_MOVE_SEL && g_chessAvailablePiecesCount > 0) {
    selectedR = g_chessAvailablePieces[g_chessSelectedPieceIdx].r;
    selectedC = g_chessAvailablePieces[g_chessSelectedPieceIdx].c;
  }

  int targetR = -1, targetC = -1;
  if (g_chessSubMode == CHESS_SUB_MOVE_SEL && g_chessPossibleMovesCount > 0) {
    targetR = g_chessPossibleMoves[g_chessSelectedMoveIdx].r2;
    targetC = g_chessPossibleMoves[g_chessSelectedMoveIdx].c2;
  }

  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      char p = g_chessBoard[r][c];
      bool isSel = (r == selectedR && c == selectedC);
      bool isTgt = (r == targetR && c == targetC);

      if (p != '.') {
        drawPiece(r, c, p, isSel, isTgt);
      } else {
        drawSquare(r, c, isSel, isTgt);
      }
    }
  }
  
  gfx.disableInversion = false; // Restore normal inversion handling

  // Info area on the left (since board is centered)
  int infoX = MARGIN_X;
  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(infoX, 15);
  u8g2.print("Chess");

  u8g2.setFont(MAIN_FONT);
  u8g2.setCursor(infoX, 29);
  u8g2.print("Rating: ");
  u8g2.print(g_chessPuzzleRating);

  u8g2.setCursor(infoX, 42);
  u8g2.print("Side: ");
  u8g2.print(g_chessPlayerIsWhite ? "White" : "Black");

  u8g2.setCursor(infoX, 55);
  u8g2.print("Tries: ");
  u8g2.print(g_chessWrongTries);

  if (g_chessSubMode == CHESS_SUB_RESULT) {
    u8g2.setFont(BOLD_FONT);
    u8g2.setCursor(infoX, 75);
    if (g_chessLast10Count > 0 && g_chessLast10Results[g_chessLast10Count - 1]) {
      u8g2.print("SOLVED!");
    } else {
      u8g2.print("FAILED!");
    }
  } else {
    u8g2.setCursor(infoX, 75);
    u8g2.print("Last 10:");
    int startX = infoX;
    int y = 84;
    for (int i = 0; i < 5; i++) {
      int rx = startX + i * 11;
      if (i < g_chessLast10Count) {
        drawResultIcon(rx, y, g_chessLast10Results[i] ? 1 : 2);
      } else {
        drawResultIcon(rx, y, 0);
      }
    }
  }

  // Draw instruction text at the bottom left
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(infoX, 114);
  if (g_chessSubMode == CHESS_SUB_PIECE_SEL) {
    u8g2.print("1xNxt 2xSel HldPrv");
  } else if (g_chessSubMode == CHESS_SUB_MOVE_SEL) {
    u8g2.print("1xNxt 2xPlay HldPrv");
  } else if (g_chessSubMode == CHESS_SUB_RESULT) {
    u8g2.print("1x Next Puzzle");
  }

  display.update();
}

void performOTAUpdate() {
  drawCenter("System Update", "Connecting to Wi-Fi...");
  if (!connectSTAWithMulti()) {
    drawCenter("System Update", "Wi-Fi failed. Press to exit.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  drawCenter("System Update", "Checking for update...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  String url = "https://raw.githubusercontent.com/felix0123456/pala/main/update.json";
  if (!http.begin(client, url)) {
    drawCenter("System Update", "Failed to connect to server.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  int code = http.GET();
  if (code != 200) {
    http.end();
    drawCenter("System Update", "Server error. Press to exit.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  String payload = http.getString();
  http.end();
  
  ALLOC_JSON_DOC(doc, 1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    drawCenter("System Update", "Parse error. Press to exit.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  String latestVer = doc["version"].as<String>();
  String binUrl = doc["binUrl"].as<String>();
  
  if (latestVer == FW_VERSION) {
    drawCenter("System Update", "Already on latest version.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  drawCenter("Downloading Update...", latestVer.c_str());
  if (!http.begin(client, binUrl)) {
    drawCenter("System Update", "Failed to open bin url.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  code = http.GET();
  if (code != 200) {
    http.end();
    drawCenter("System Update", "Download error.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  int contentLength = http.getSize();
  if (contentLength <= 0) {
    http.end();
    drawCenter("System Update", "Invalid file size.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  if (!Update.begin(contentLength, U_FLASH)) {
    http.end();
    drawCenter("System Update", "Not enough space.");
    while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
    exitSpotifyMode();
    return;
  }
  
  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  
  if (written == contentLength) {
    if (Update.end()) {
      drawCenter("System Update", "Success! Rebooting...");
      delay(2000);
      ESP.restart();
    } else {
      drawCenter("System Update", "Update failed.");
    }
  } else {
    drawCenter("System Update", "Download interrupted.");
  }
  
  while (!btns.b1.shortClick && !btns.b2.shortClick && !btns.b2.shortClick) { delay(10); btns.poll(); }
  exitSpotifyMode();
}

void initChessGame() {
  markUserActivity();
  WiFi.mode(WIFI_STA);
  loadWifiProfiles();
  
  if (fetchChessPuzzle()) {
    g_chessSubMode = CHESS_SUB_PIECE_SEL;
    g_chessAvailablePiecesCount = getAvailablePieces(g_chessBoard, g_chessPlayerIsWhite, g_chessAvailablePieces);
    g_chessSelectedPieceIdx = 0;
    drawChessScreen();
  } else {
    mode = MODE_LIBRARY;
    drawLibrary();
  }
  resetInputFrontend();
}

void handleModeChess() {
  if (g_chessSubMode == CHESS_SUB_RESULT) {
    if (btns.b1.shortClick || btns.b1.doubleClick) {
      if (fetchChessPuzzle()) {
        g_chessSubMode = CHESS_SUB_PIECE_SEL;
        g_chessAvailablePiecesCount = getAvailablePieces(g_chessBoard, g_chessPlayerIsWhite, g_chessAvailablePieces);
        g_chessSelectedPieceIdx = 0;
        drawChessScreen();
      } else {
        mode = MODE_LIBRARY;
        drawLibrary();
      }
      resetInputFrontend();
      return;
    }
    if (btns.b2.doubleClick) {
      mode = MODE_LIBRARY;
      drawLibrary();
      resetInputFrontend();
      return;
    }
    return;
  }

  if (g_chessSubMode == CHESS_SUB_PIECE_SEL) {
    if (btns.b1.shortClick) {
      if (g_chessAvailablePiecesCount > 0) {
        g_chessSelectedPieceIdx++;
        if (g_chessSelectedPieceIdx >= g_chessAvailablePiecesCount) {
          g_chessSelectedPieceIdx = 0;
        }
        drawChessScreen();
      }
      return;
    }

    if (btns.b1.longClick) { // Cycle back
      if (g_chessAvailablePiecesCount > 0) {
        g_chessSelectedPieceIdx--;
        if (g_chessSelectedPieceIdx < 0) {
          g_chessSelectedPieceIdx = g_chessAvailablePiecesCount - 1;
        }
        drawChessScreen();
      }
      return;
    }

    if (btns.b2.shortClick) { // Cycle back
      if (g_chessAvailablePiecesCount > 0) {
        g_chessSelectedPieceIdx--;
        if (g_chessSelectedPieceIdx < 0) {
          g_chessSelectedPieceIdx = g_chessAvailablePiecesCount - 1;
        }
        drawChessScreen();
      }
      return;
    }

    if (btns.b2.doubleClick) {
      mode = MODE_LIBRARY;
      drawLibrary();
      resetInputFrontend();
      return;
    }

    if (btns.b1.doubleClick) {
      if (g_chessAvailablePiecesCount > 0) {
        int r = g_chessAvailablePieces[g_chessSelectedPieceIdx].r;
        int c = g_chessAvailablePieces[g_chessSelectedPieceIdx].c;
        g_chessPossibleMovesCount = getPseudoMoves(g_chessBoard, r, c, g_chessPossibleMoves);
        if (g_chessPossibleMovesCount > 0) {
          g_chessSelectedMoveIdx = 0;
          g_chessSubMode = CHESS_SUB_MOVE_SEL;
          drawChessScreen();
        } else {
          showToast("No moves for piece");
          drawChessScreen();
        }
      }
      return;
    }
  }

  else if (g_chessSubMode == CHESS_SUB_MOVE_SEL) {
    if (btns.b1.shortClick) {
      g_chessSelectedMoveIdx++;
      if (g_chessSelectedMoveIdx >= g_chessPossibleMovesCount) {
        g_chessSubMode = CHESS_SUB_PIECE_SEL;
      }
      drawChessScreen();
      return;
    }

    if (btns.b1.longClick) { // Cycle backward
      g_chessSelectedMoveIdx--;
      if (g_chessSelectedMoveIdx < 0) {
        g_chessSubMode = CHESS_SUB_PIECE_SEL; // Cycle back past 0 returns to piece selection
      }
      drawChessScreen();
      return;
    }

    if (btns.b2.shortClick) { // Cycle backward
      g_chessSelectedMoveIdx--;
      if (g_chessSelectedMoveIdx < 0) {
        g_chessSubMode = CHESS_SUB_PIECE_SEL; // Cycle back past 0 returns to piece selection
      }
      drawChessScreen();
      return;
    }

    if (btns.b2.doubleClick) {
      g_chessSubMode = CHESS_SUB_PIECE_SEL;
      drawChessScreen();
      return;
    }

    if (btns.b1.doubleClick) {
      ChessMove m = g_chessPossibleMoves[g_chessSelectedMoveIdx];
      String playerMove = String((char)('a' + m.c1)) + String((char)('8' - m.r1)) +
                          String((char)('a' + m.c2)) + String((char)('8' - m.r2));
      
      bool isPromo = (g_chessSolution[g_chessCurrentMoveIdx].length() == 5);
      String matchMove = isPromo ? g_chessSolution[g_chessCurrentMoveIdx].substring(0, 4) : g_chessSolution[g_chessCurrentMoveIdx];
      
      if (playerMove == matchMove) {
        char piece = g_chessBoard[m.r1][m.c1];
        if (isPromo) {
          char promoType = g_chessSolution[g_chessCurrentMoveIdx][4];
          piece = g_chessPlayerIsWhite ? (promoType - 32) : promoType;
        }
        g_chessBoard[m.r2][m.c2] = piece;
        g_chessBoard[m.r1][m.c1] = '.';
        
        drawChessScreen();
        
        if (g_chessCurrentMoveIdx + 1 >= g_chessSolutionCount) {
          g_chessSubMode = CHESS_SUB_RESULT;
          bool finalSuccess = !g_chessAlreadyFailed;
          markPuzzleCompleted(g_chessPuzzleId);
          if (finalSuccess) {
            rtc_chessPuzzleSolvedToday = true;
          }
          if (g_chessLast10Count < 10) {
            g_chessLast10Results[g_chessLast10Count++] = finalSuccess;
          } else {
            for (int i = 0; i < 9; i++) g_chessLast10Results[i] = g_chessLast10Results[i+1];
            g_chessLast10Results[9] = finalSuccess;
          }
          drawChessScreen();
        } else {
          delay(800);
          int or1, oc1, or2, oc2;
          uciToCoords(g_chessSolution[g_chessCurrentMoveIdx + 1], or1, oc1, or2, oc2);
          
          bool oppPromo = (g_chessSolution[g_chessCurrentMoveIdx + 1].length() == 5);
          char oppPiece = g_chessBoard[or1][oc1];
          if (oppPromo) {
            char oppPromoType = g_chessSolution[g_chessCurrentMoveIdx + 1][4];
            oppPiece = (!g_chessPlayerIsWhite) ? (oppPromoType - 32) : oppPromoType;
          }
          g_chessBoard[or2][oc2] = oppPiece;
          g_chessBoard[or1][oc1] = '.';
          
          g_chessCurrentMoveIdx += 2;
          g_chessSubMode = CHESS_SUB_PIECE_SEL;
          g_chessAvailablePiecesCount = getAvailablePieces(g_chessBoard, g_chessPlayerIsWhite, g_chessAvailablePieces);
          g_chessSelectedPieceIdx = 0;
          drawChessScreen();
        }
      } else {
        g_chessAlreadyFailed = true;
        g_chessWrongTries++;
        
        if (g_chessWrongTries >= 3) {
          g_chessSubMode = CHESS_SUB_RESULT;
          markPuzzleCompleted(g_chessPuzzleId);
          if (g_chessLast10Count < 10) {
            g_chessLast10Results[g_chessLast10Count++] = false;
          } else {
            for (int i = 0; i < 9; i++) g_chessLast10Results[i] = g_chessLast10Results[i+1];
            g_chessLast10Results[9] = false;
          }
          drawChessScreen();
        } else {
          showToast("Wrong! Try again");
          g_chessSubMode = CHESS_SUB_PIECE_SEL;
          g_chessAvailablePiecesCount = getAvailablePieces(g_chessBoard, g_chessPlayerIsWhite, g_chessAvailablePieces);
          g_chessSelectedPieceIdx = 0;
          drawChessScreen();
        }
      }
      return;
    }
  }
}

// ============================================================================
//  Setup / Mode handlers / Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  setCpuFrequencyMhz(240);

  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN1), btnISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN2), btnISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN3), btnISR, CHANGE);

  u8g2.begin(gfx);

  invalidateMetrics();
  (void)getMetrics();

  if (HAS_BATTERY) {
    adcSetupOnce();
    pinMode(BAT_ADC_CTRL, INPUT);
    updateBatteryCached(true);
  }

  display.fastmodeOff();
  display.clear();

  if (!fsBegin()) {
    drawCenter("FS mount failed", "Reflash / reset needed");
    return;
  }
  ensureBooksDir();

  {
    uint64_t chipId = ESP.getEfuseMac();
    snprintf(AP_SSID, sizeof(AP_SSID), "PALA-%06llX", chipId & 0xFFFFFFULL);
  }

  prefs.begin("ereader", false);
  loadSettings();
  loadBooks();

  markUserActivity();

  bool restored = false;
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();

  if (rtc_inSpotifyScreensaver) {
    if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
      if (ensureSpotifyWiFi()) {
        SpotifyTrackInfo track = getSpotifyCurrentlyPlaying();
        String stateStr = track.trackName + "|" + track.artistName + "|" + String(track.isPlaying) + "|" + track.coverUrl;
        uint32_t currentHash = fnv1a32(stateStr.c_str());
        if (currentHash != rtc_lastSpotifyHash) {
          rtc_lastSpotifyHash = currentHash;
          drawSpotifyScreen(track);
        }
      }
      delay(200);
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      delay(50);
      esp_wifi_stop();
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN1, 0);
      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
    } else {
      rtc_inSpotifyScreensaver = false;
      mode = MODE_SPOTIFY;
      g_spotifyForceRefresh = true;
      resetInputFrontend();
      restored = true;
    }
  }

  if (rtc_inClockScreensaver) {
    if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
      // Connect to WiFi once a day (if year is invalid or every 144 wakeups/24h) to resync NTP
      static RTC_DATA_ATTR int wakeupCount = 0;
      wakeupCount++;
      if (wakeupCount >= 144) {
        wakeupCount = 0;
        if (connectSTAWithMulti()) {
          syncNTP();
          WiFi.disconnect(true, true);
          WiFi.mode(WIFI_OFF);
          delay(50);
          esp_wifi_stop();
        }
      }
      drawClockScreen();
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN1, 0);
      esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
      esp_deep_sleep_start();
    } else {
      rtc_inClockScreensaver = false;
      mode = MODE_LIBRARY;
      resetInputFrontend();
      restored = false; // Draw library normally
    }
  }

  if (!restored && prefs.getInt("wake_mode", 0) == 1) {
    String wp = prefs.getString("wake_path", "");
    if (wp.length() > 0) {
      for (int i = 0; i < bookCount; i++) {
        if (strcmp(books[i].path, wp.c_str()) == 0) {
          if (openBookByIndex(i)) {
            resetPreviewState();
            mode = MODE_READER;
            resetInputFrontend();
            pageTurnsSinceFull = FULL_REFRESH_EVERY_N_PAGES;
            renderCurrentPage();
            restored = true;
          }
          break;
        }
      }
    }
  }

  if (!restored) {
    syncWithCloud();
    loadBooks();
    drawLibrary();
    resetInputFrontend();
  }
}

static void handleModeUpload() {
  server.handleClient();
  bool timeout = (uint32_t)(millis() - g_uploadStartMs) > UPLOAD_AUTO_EXIT_MS;
  if (btns.b1.shortClick || timeout) stopUploadModeToLibrary();
}

static void handleModeAbout() {
  if (btns.b1.shortClick || btns.b1.doubleClick || btns.b1.longClick || btns.b1.quadClick) {
    mode = MODE_SETTINGS;
    drawSettings();
  }
  if (btns.b2.doubleClick || btns.b2.shortClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
  }
}

static void handleModeBookmarkBookSelect() {
  if (bookCount == 0) {
    if (btns.b1.anyClick()) {
      mode = MODE_LIBRARY;
      drawLibrary();
    }
    return;
  }

  if (btns.b1.shortClick) {
    bmBookIndex++;
    if (bmBookIndex >= bookCount) bmBookIndex = 0;
    drawBookmarksBookSelect();
    return;
  }

  if (btns.b1.longClick) {
    bmBookIndex--;
    if (bmBookIndex < 0) bmBookIndex = bookCount - 1;
    drawBookmarksBookSelect();
    return;
  }

  if (btns.b2.shortClick) {
    bmBookIndex--;
    if (bmBookIndex < 0) bmBookIndex = bookCount - 1;
    drawBookmarksBookSelect();
    return;
  }

  if (btns.b1.doubleClick) {
    bmSelIndex = 0;
    mode = MODE_BM_LIST;
    drawBookmarksList();
  }

  if (btns.b2.doubleClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
    return;
  }
}

static void handleModeBookmarkList() {
  String key = prefKeyForBook(String(books[bmBookIndex].path));
  bmCount = loadBookmarksForKey(key, bmPages);
  if (bmSelIndex >= (int)bmCount) bmSelIndex = max(0, (int)bmCount - 1);

  if (btns.b1.shortClick) {
    if (bmCount > 0) {
      bmSelIndex++;
      if (bmSelIndex >= (int)bmCount) bmSelIndex = 0;
    }
    drawBookmarksList();
    return;
  }

  if (btns.b2.shortClick) {
    if (bmCount > 0) {
      bmSelIndex--;
      if (bmSelIndex < 0) bmSelIndex = bmCount - 1;
    }
    drawBookmarksList();
    return;
  }

  if (btns.b1.doubleClick) {
    if (bmCount == 0) return;

    String previewPath = String(books[bmBookIndex].path);

    if (currentBookPath == previewPath && currentBookKey.length() > 0) {
      bmPreviewSavedPage = pageIndex;
    } else {
      String previewKey = prefKeyForBook(previewPath);
      bmPreviewSavedPage = prefs.getInt((previewKey + "_p").c_str(), 0);
    }

    if (openBookByIndex(bmBookIndex)) {
      bmPreviewActive = true;
      pageIndex = (int)bmPages[bmSelIndex];
      if (pageIndex < 0) pageIndex = 0;
      mode = MODE_BM_PREVIEW;
      renderCurrentPage();
    } else {
      mode = MODE_LIBRARY;
      drawLibrary();
    }
    return;
  }

  if (btns.b2.doubleClick) {
    mode = MODE_BM_BOOK_SELECT;
    drawBookmarksBookSelect();
    return;
  }
}

static void handleModeBookmarkPreview() {
  if (btns.b1.tripleClick) {
    bmPreviewActive = false;
    pageIndex = bmPreviewSavedPage;
    mode = MODE_READER;
    renderCurrentPage();
    return;
  }

  if (btns.b1.longClick) {
    bmPreviewActive = false;
    saveProgressThrottled(true);
    mode = MODE_READER;
    renderCurrentPage();
    return;
  }

  if (btns.b2.doubleClick) {
    bmPreviewActive = false;
    pageIndex = bmPreviewSavedPage;
    mode = MODE_READER;
    renderCurrentPage();
    return;
  }

  if (btns.b1.doubleClick) {
    pageIndex--;
    if (pageIndex < 0) pageIndex = 0;
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.b2.shortClick) {
    pageIndex--;
    if (pageIndex < 0) pageIndex = 0;
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.b1.shortClick) {
    pageIndex++;
    ensureOffsetsUpTo(pageIndex);
    if (eofReached && pageIndex >= knownPages) pageIndex = knownPages - 1;
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }
}

static void handleModeLibrary() {
  buildLibraryEntries();
  int totalItems = libraryEntryCount;

  if (btns.b1.shortClick) {
    selectedItem++;
    if (selectedItem >= totalItems) selectedItem = 0;
    drawLibrary();
    return;
  }

  if (btns.b2.shortClick) {
    selectedItem--;
    if (selectedItem < 0) selectedItem = totalItems - 1;
    drawLibrary();
    return;
  }

  if (btns.b2.doubleClick) {
    currentLibraryFolder = folderParent(currentLibraryFolder);
    selectedItem = 0;
    drawLibrary();
    return;
  }

  if (!btns.b1.doubleClick) return;

  if (selectedItem < 0 || selectedItem >= libraryEntryCount) {
    drawLibrary();
    return;
  }

  LibraryEntryType entryType = libraryEntryTypes[selectedItem];
  int entryRef = libraryEntryRefs[selectedItem];

  if (entryType == LIB_ENTRY_FOLDER) {
    currentLibraryFolder = String(folders[entryRef]);
    selectedItem = 0;
    drawLibrary();
    return;
  }

  if (entryType == LIB_ENTRY_BOOK) {
    if (openBookByIndex(entryRef)) {
      bmPreviewActive = false;
      mode = MODE_READER;
      renderCurrentPage();
    } else {
      drawCenter("Open failed", "Try upload again");
      delay(600);
      drawLibrary();
    }
    return;
  }

  if (entryType == LIB_ENTRY_BOOKMARKS) {
    bmBookIndex = 0;
    mode = MODE_BM_BOOK_SELECT;
    drawBookmarksBookSelect();
    return;
  }

  if (entryType == LIB_ENTRY_TODO) {
    mode = MODE_TODO;
    g_todoSelectedIdx = 0;
    loadTodoList();
    drawTodoList();
    return;
  }

  if (entryType == LIB_ENTRY_CALENDAR) {
    mode = MODE_CALENDAR;
    drawCenter("Calendar", "Updating events...");
    fetchGoogleCalendar();
    drawCalendarScreen();
    return;
  }

  if (entryType == LIB_ENTRY_SPOTIFY) {
    mode = MODE_SPOTIFY;
    g_spotifyForceRefresh = true;
    resetInputFrontend();
    return;
  }

  if (entryType == LIB_ENTRY_SETTINGS) {
    selectedSettingItem = 1;
    mode = MODE_SETTINGS;
    drawSettings();
    return;
  }

  if (entryType == LIB_ENTRY_CHESS) {
    mode = MODE_CHESS;
    initChessGame();
    return;
  }
}

void jumpBookOffset(int percentChange) {
  if (!bookFile) return;
  size_t total = bookFile.size();
  if (total == 0) return;

  uint32_t currentOff = pageOffsets[pageIndex];
  int32_t change = (total * percentChange) / 100;
  int32_t targetOff = (int32_t)currentOff + change;

  uint32_t startOffset = findBookContentStart(bookFile);
  if (targetOff < (int32_t)startOffset) targetOff = (int32_t)startOffset;
  if (targetOff >= (int32_t)total) {
    targetOff = (int32_t)total - 500;
    if (targetOff < (int32_t)startOffset) targetOff = (int32_t)startOffset;
  }

  pageIndex = 0;
  knownPages = 1;
  pageOffsets[0] = (uint32_t)targetOff;
  eofReached = false;

  saveProgressThrottled(true);
}

static void handleModeReader() {
  if (btns.b1.quadClick) {
    g_jumpModeActive = !g_jumpModeActive;
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (g_jumpModeActive) {
    if (btns.b1.longClick) {
      // Hold in jump mode switches to Chapter Selection Menu!
      mode = MODE_CHAPTER_LIST;
      g_selectedChapterIdx = 0;
      drawChapterList();
      return;
    }
    if (btns.b1.doubleClick) {
      jumpBookOffset(-10);
      pageTurnsSinceFull++;
      renderCurrentPage();
      return;
    }
    if (btns.b1.shortClick) {
      jumpBookOffset(10);
      pageTurnsSinceFull++;
      renderCurrentPage();
      return;
    }
    return;
  }

  if (btns.b1.longClick) {
    const char* msg = addBookmarkForCurrentBook();
    if (msg) showToast(msg);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.b2.doubleClick) {
    enterLibraryRoot(true);
    resetInputFrontend();
    return;
  }

  if (btns.b1.doubleClick) {
    if (pageIndex > 0) pageIndex--;
    saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.b2.shortClick) {
    if (pageIndex > 0) pageIndex--;
    saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.b1.shortClick) {
    int oldPage = pageIndex;
    pageIndex++;
    ensureOffsetsUpTo(pageIndex);
    if (eofReached && pageIndex >= knownPages) pageIndex = knownPages - 1;
    if (pageIndex < 0) pageIndex = 0;
    if (pageIndex != oldPage) saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }
}

void handleModeSpotify() {
  if (!ensureSpotifyWiFi()) {
    exitSpotifyMode();
    return;
  }

  static uint32_t lastPollMs = 0;
  uint32_t now = millis();
  bool needPoll = (lastPollMs == 0 || (now - lastPollMs) >= 5000 || g_spotifyForceRefresh);

  if (needPoll) {
    g_spotifyForceRefresh = false;
    lastPollMs = now;
    SpotifyTrackInfo track = getSpotifyCurrentlyPlaying();
    String stateStr = track.trackName + "|" + track.artistName + "|" + String(track.isPlaying) + "|" + track.coverUrl;
    uint32_t currentHash = fnv1a32(stateStr.c_str());

    if (currentHash != rtc_lastSpotifyHash) {
      rtc_lastSpotifyHash = currentHash;
      drawSpotifyScreen(track);
    }
  }

  if (btns.b1.shortClick) {
    spotifyNextTrack();
    g_spotifyForceRefresh = true;
  }
  else if (btns.b1.doubleClick || btns.b2.shortClick) {
    spotifyPrevTrack();
    g_spotifyForceRefresh = true;
  }
  else if (btns.b2.doubleClick) {
    exitSpotifyMode();
  }
  else if (btns.b1.longClick) {
    SpotifyTrackInfo track = getSpotifyCurrentlyPlaying();
    spotifyTogglePlayPause(track.isPlaying);
    g_spotifyForceRefresh = true;
  }
}

// ============================================================================
//  Todo List Functions
// ============================================================================
void loadTodoList() {
  g_todoCount = 0;
  if (!FS.exists("/todo.txt")) {
    File f = FS.open("/todo.txt", "w");
    if (f) {
      f.println("[ ] Buy milk");
      f.println("[ ] Read book");
      f.println("[x] Flash PalaOne");
      f.close();
    }
  }
  File f = FS.open("/todo.txt", "r");
  if (!f) return;
  while (f.available() && g_todoCount < MAX_TODO_ITEMS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    bool checked = false;
    String text = line;
    if (line.startsWith("[x] ") || line.startsWith("[X] ")) {
      checked = true;
      text = line.substring(4);
    } else if (line.startsWith("[ ] ")) {
      checked = false;
      text = line.substring(4);
    }
    text.trim();
    g_todoItems[g_todoCount] = {checked, text};
    g_todoCount++;
  }
  f.close();
}

void saveTodoList() {
  File f = FS.open("/todo.txt", "w");
  if (!f) return;
  for (int i = 0; i < g_todoCount; i++) {
    String prefix = g_todoItems[i].checked ? "[x] " : "[ ] ";
    f.println(prefix + g_todoItems[i].text);
  }
  f.close();
}

void drawTodoList() {
  prepareMenuFrame();
  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP + 1;
  int y = drawSectionHeader("Todo List", true);

  if (g_todoCount == 0) {
    drawMenuBulletRow(y, "No tasks", true, false);
    display.update();
    return;
  }

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 1) visible = 1;
  if (visible > 5) visible = 5;

  int top = g_todoSelectedIdx - (visible / 2);
  if (top < 0) top = 0;
  if (top > g_todoCount - visible) top = max(0, g_todoCount - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= g_todoCount) break;

    bool selected = (idx == g_todoSelectedIdx);
    int cbX = UI_LIST_LEFT + (selected ? UI_INDENT_SELECTED : 0);

    drawResultIcon(cbX, y - 9, g_todoItems[idx].checked ? 1 : 0);

    if (selected) {
      gfx.fillCircle(UI_BULLET_X + UI_BULLET_R, y - 4, UI_BULLET_R, 1);
    }

    u8g2.setCursor(cbX + 14, y);
    u8g2.print(g_todoItems[idx].text.c_str());

    y += lineH;
  }

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(MARGIN_X, H - 2);
  u8g2.print("1x Next  2x Toggle  3x Back");
  u8g2.setFont(MAIN_FONT);

  display.update();
}

void handleModeTodo() {
  if (btns.b1.shortClick) {
    if (g_todoCount > 0) {
      g_todoSelectedIdx++;
      if (g_todoSelectedIdx >= g_todoCount) g_todoSelectedIdx = 0;
      drawTodoList();
    }
    return;
  }

  if (btns.b2.shortClick) {
    if (g_todoCount > 0) {
      g_todoSelectedIdx--;
      if (g_todoSelectedIdx < 0) g_todoSelectedIdx = g_todoCount - 1;
      drawTodoList();
    }
    return;
  }

  if (btns.b2.doubleClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
    return;
  }

  if (btns.b1.doubleClick) {
    if (g_todoCount > 0) {
      g_todoItems[g_todoSelectedIdx].checked = !g_todoItems[g_todoSelectedIdx].checked;
      saveTodoList();
      drawTodoList();
    }
    return;
  }
}

// ============================================================================
//  Calendar Functions
// ============================================================================
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Syncing clock via NTP...");
  configTime(g_timezoneOffsetHours * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 6000)) {
    Serial.println("Time resynced!");
  }
}

time_t parseIcsDateTime(const String& dt) {
  if (dt.length() < 8) return 0;
  struct tm tm_ev = {0};
  tm_ev.tm_year = dt.substring(0, 4).toInt() - 1900;
  tm_ev.tm_mon = dt.substring(4, 6).toInt() - 1;
  tm_ev.tm_mday = dt.substring(6, 8).toInt();
  if (dt.length() >= 15 && dt[8] == 'T') {
    tm_ev.tm_hour = dt.substring(9, 11).toInt();
    tm_ev.tm_min = dt.substring(11, 13).toInt();
    tm_ev.tm_sec = dt.substring(13, 15).toInt();
  }
  time_t t = mktime(&tm_ev);
  if (dt.endsWith("Z")) {
    t += g_timezoneOffsetHours * 3600;
  }
  return t;
}

String formatEventTime(time_t t, bool allDay) {
  struct tm* tm_ev = localtime(&t);
  char buf[32];
  if (allDay) {
    strftime(buf, sizeof(buf), "%a, %b %d (All day)", tm_ev);
  } else {
    strftime(buf, sizeof(buf), "%a, %b %d at %H:%M", tm_ev);
  }
  return String(buf);
}

bool fetchGoogleCalendar() {
  g_calEventCount = 0;
  if (g_calUrl.length() == 0) return false;

  if (WiFi.status() != WL_CONNECTED) {
    if (!connectSTAWithMulti()) return false;
  }
  syncNTP();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  time_t nowTime;
  time(&nowTime);

  int startIndex = 0;
  while (startIndex < g_calUrl.length()) {
    int commaIndex = g_calUrl.indexOf(',', startIndex);
    String url = "";
    if (commaIndex == -1) {
      url = g_calUrl.substring(startIndex);
      startIndex = g_calUrl.length();
    } else {
      url = g_calUrl.substring(startIndex, commaIndex);
      startIndex = commaIndex + 1;
    }
    url.trim();
    if (url.length() == 0) continue;

    if (!http.begin(client, url)) continue;

    int httpCode = http.GET();
    if (httpCode != 200) {
      http.end();
      continue;
    }

    WiFiClient* stream = http.getStreamPtr();
    String line = "";
    line.reserve(128);
    bool inEvent = false;
    String currentSummary = "";
    String currentStart = "";

    char peekChar = ' ';
    if (stream->available()) peekChar = stream->peek();

    if (peekChar != 'B' && peekChar != 'b') {
      while (stream->available() && g_calEventCount < MAX_CAL_EVENTS) {
        String evLine = stream->readStringUntil('\n');
        evLine.trim();
        if (evLine.length() == 0) continue;
        
        int splitIdx = evLine.indexOf('|');
        if (splitIdx != -1) {
          g_calEvents[g_calEventCount].title = evLine.substring(0, splitIdx);
          g_calEvents[g_calEventCount].dateStr = evLine.substring(splitIdx + 1);
        } else {
          g_calEvents[g_calEventCount].title = evLine;
          g_calEvents[g_calEventCount].dateStr = "Upcoming";
        }
        g_calEvents[g_calEventCount].startTime = nowTime + g_calEventCount;
        g_calEventCount++;
      }
      http.end();
      continue;
    }

    while (stream->available() && g_calEventCount < MAX_CAL_EVENTS) {
      char c = stream->read();
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line == "BEGIN:VEVENT") {
          inEvent = true;
          currentSummary = "";
          currentStart = "";
        } else if (line == "END:VEVENT") {
          if (inEvent && currentStart.length() >= 8) {
            time_t eventTime = parseIcsDateTime(currentStart);
            if (eventTime >= nowTime - 3600 * 24) {
              String dateStr = formatEventTime(eventTime, currentStart.length() <= 8);

              int insertIdx = g_calEventCount;
              for (int i = 0; i < g_calEventCount; i++) {
                if (eventTime < g_calEvents[i].startTime) {
                  insertIdx = i;
                  break;
                }
              }
              if (insertIdx < MAX_CAL_EVENTS) {
                for (int i = g_calEventCount; i > insertIdx; i--) {
                  if (i < MAX_CAL_EVENTS) g_calEvents[i] = g_calEvents[i-1];
                }
                g_calEvents[insertIdx].title = currentSummary;
                g_calEvents[insertIdx].dateStr = dateStr;
                g_calEvents[insertIdx].startTime = eventTime;
                if (g_calEventCount < MAX_CAL_EVENTS) g_calEventCount++;
              }
            }
          }
          inEvent = false;
        } else if (inEvent) {
          if (line.startsWith("SUMMARY:")) {
            currentSummary = line.substring(8);
          } else if (line.startsWith("DTSTART")) {
            int colonIdx = line.indexOf(':');
            if (colonIdx != -1) {
              currentStart = line.substring(colonIdx + 1);
            }
          }
        }
        line = "";
      } else {
        if (line.length() < 128) line += c;
      }
    }
    http.end();
  }

  return true;
}

void drawCalendarScreen() {
  prepareMenuFrame();
  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP + 1;
  int y = drawSectionHeader("Calendar", true);

  if (g_calUrl.length() == 0) {
    drawMenuBulletRow(y, "Configure URL in Web Dashboard", true, false);
    display.update();
    return;
  }

  if (g_calEventCount == 0) {
    drawMenuBulletRow(y, "No upcoming events", true, false);
    display.update();
    return;
  }

  int visible = (H - y - BOT_PAD) / (lineH * 2 - 2);
  if (visible < 1) visible = 1;
  if (visible > 3) visible = 3;

  for (int i = 0; i < visible && i < g_calEventCount; i++) {
    u8g2.setFont(BOLD_FONT);
    u8g2.setCursor(MARGIN_X + 4, y);
    String title = g_calEvents[i].title;
    if (u8g2.getUTF8Width(title.c_str()) > W - MARGIN_X - 10) {
      while (title.length() > 0 && u8g2.getUTF8Width((title + "...").c_str()) > W - MARGIN_X - 10) {
        title.remove(title.length() - 1);
      }
      title += "...";
    }
    u8g2.print(title.c_str());
    y += lineH - 2;

    u8g2.setFont(MAIN_FONT);
    u8g2.setCursor(MARGIN_X + 12, y);
    u8g2.print(g_calEvents[i].dateStr.c_str());
    y += lineH + 2;
  }

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(MARGIN_X, H - 2);
  u8g2.print("1x Refresh  3x Back");
  u8g2.setFont(MAIN_FONT);

  display.update();
}

void handleModeCalendar() {
  if (btns.b1.shortClick || btns.b1.doubleClick) {
    drawCenter("Calendar", "Updating events...");
    fetchGoogleCalendar();
    drawCalendarScreen();
  }
  if (btns.b2.doubleClick || btns.b2.shortClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
  }
}

// ============================================================================
//  Structural Chapter Parser
// ============================================================================
bool isChapterHeader(const String& s) {
  String lower = s;
  lower.toLowerCase();
  if (lower.indexOf("chapter") != -1 || lower.indexOf("kapitel") != -1) {
    return true;
  }
  
  // All Caps check
  int alphaCount = 0;
  bool allCaps = true;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') {
      allCaps = false;
      break;
    }
    if (c >= 'A' && c <= 'Z') {
      alphaCount++;
    }
  }
  if (allCaps && alphaCount >= 4) {
    if (s.indexOf("GUTENBERG") == -1 && s.indexOf("PRODUCER") == -1 && s.indexOf("DISTRIBUTOR") == -1) {
      return true;
    }
  }
  return false;
}

void buildTableOfContents() {
  g_chapterCount = 0;
  if (!bookFile) return;

  uint32_t prevPos = bookFile.position();
  uint32_t startOffset = findBookContentStart(bookFile);
  bookFile.seek(startOffset);

  String line = "";
  line.reserve(128);
  uint32_t lineStartOffset = startOffset;

  while (bookFile.available() && g_chapterCount < MAX_CHAPTERS) {
    uint32_t currentPos = bookFile.position();
    char c = (char)bookFile.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line.trim();
      if (line.length() >= 3 && line.length() < 80) {
        if (isChapterHeader(line)) {
          g_chapters[g_chapterCount].offset = lineStartOffset;
          String title = line;
          if (title.length() > 31) title = title.substring(0, 28) + "...";
          strncpy(g_chapters[g_chapterCount].title, title.c_str(), 31);
          g_chapters[g_chapterCount].title[31] = '\0';
          g_chapterCount++;
        }
      }
      line = "";
      lineStartOffset = bookFile.position();
    } else {
      line += c;
    }
  }

  line.trim();
  if (line.length() >= 3 && line.length() < 80 && g_chapterCount < MAX_CHAPTERS) {
    if (isChapterHeader(line)) {
      g_chapters[g_chapterCount].offset = lineStartOffset;
      String title = line;
      if (title.length() > 31) title = title.substring(0, 28) + "...";
      strncpy(g_chapters[g_chapterCount].title, title.c_str(), 31);
      g_chapters[g_chapterCount].title[31] = '\0';
      g_chapterCount++;
    }
  }

  bookFile.seek(prevPos);
  Serial.printf("TOC Parser: Found %d chapters.\n", g_chapterCount);
}

void drawChapterList() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP + 1;

  int y = drawSectionHeader("Chapters", true);

  if (g_chapterCount == 0) {
    drawMenuBulletRow(y, "No chapters found", true, false);
    display.update();
    return;
  }

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 1) visible = 1;
  if (visible > 5) visible = 5;

  int top = g_selectedChapterIdx - (visible / 2);
  if (top < 0) top = 0;
  if (top > g_chapterCount - visible) top = max(0, g_chapterCount - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= g_chapterCount) break;

    drawMenuBulletRow(y, String(g_chapters[idx].title), idx == g_selectedChapterIdx, idx == g_selectedChapterIdx);
    y += lineH;
  }
  
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(MARGIN_X, H - 2);
  u8g2.print("1x Next  2x Select  Hold Back");
  u8g2.setFont(MAIN_FONT);

  display.update();
}

void handleModeChapterList() {
  if (btns.b1.shortClick) {
    if (g_chapterCount > 0) {
      g_selectedChapterIdx++;
      if (g_selectedChapterIdx >= g_chapterCount) g_selectedChapterIdx = 0;
      drawChapterList();
    }
    return;
  }

  if (btns.b2.shortClick) {
    if (g_chapterCount > 0) {
      g_selectedChapterIdx--;
      if (g_selectedChapterIdx < 0) g_selectedChapterIdx = g_chapterCount - 1;
      drawChapterList();
    }
    return;
  }

  if (btns.b2.doubleClick) {
    mode = MODE_READER;
    g_jumpModeActive = true;
    renderCurrentPage();
    return;
  }

  if (btns.b1.doubleClick) {
    if (g_chapterCount > 0) {
      jumpToChapter(g_selectedChapterIdx);
    }
    return;
  }

  if (btns.b1.longClick) {
    mode = MODE_READER;
    g_jumpModeActive = true;
    renderCurrentPage();
    return;
  }
}

void jumpToChapter(int idx) {
  if (idx < 0 || idx >= g_chapterCount) return;
  uint32_t offset = g_chapters[idx].offset;
  pageIndex = 0;
  knownPages = 1;
  pageOffsets[0] = offset;
  eofReached = false;
  saveProgressThrottled(true);
  mode = MODE_READER;
  g_jumpModeActive = false;
  pageTurnsSinceFull = FULL_REFRESH_EVERY_N_PAGES;
  renderCurrentPage();
}

// ============================================================================
//  Clock Screensaver
// ============================================================================
void drawClockScreen() {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();

  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }

  time_t nowTime;
  time(&nowTime);
  struct tm* timeinfo = localtime(&nowTime);

  char timeBuf[16];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M", timeinfo);

  char dateBuf[32];
  strftime(dateBuf, sizeof(dateBuf), "%A, %b %d", timeinfo);

  u8g2.setFont(u8g2_font_helvB24_tf);
  int timeW = u8g2.getUTF8Width(timeBuf);
  u8g2.setCursor((W - timeW) / 2, 55);
  u8g2.print(timeBuf);

  u8g2.setFont(u8g2_font_helvR12_tf);
  int dateW = u8g2.getUTF8Width(dateBuf);
  u8g2.setCursor((W - dateW) / 2, 85);
  u8g2.print(dateBuf);

  #if HAS_BATTERY
  updateBatteryCached(true);
  char batBuf[16];
  snprintf(batBuf, sizeof(batBuf), "%d%%", g_batValid ? g_batPctShown : 0);
  u8g2.setFont(u8g2_font_5x8_tf);
  int batW = u8g2.getUTF8Width(batBuf);
  u8g2.setCursor(W - MARGIN_X - batW, H - 4);
  u8g2.print(batBuf);
  #endif

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(MARGIN_X, H - 4);
  u8g2.print("Updates every 10 min");

  display.update();
}

void drawDailyBoardScreensaver() {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();

  if (g_nightMode) {
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        display.drawPixel(x, y, BLACK);
      }
    }
  }

  // Draw board grid
  gfx.disableInversion = true;
  for (int i = 0; i <= 8; i++) {
    gfx.drawFastHLine(65, 1 + i * 15, 120, 1);
    gfx.drawFastVLine(65 + i * 15, 1, 120, 1);
  }

  // Draw squares & pieces
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      char p = g_chessBoard[r][c];
      int x = 65 + c * 15;
      int y = 1 + r * 15;
      bool isDarkSquare = ((r + c) % 2 == 1);
      if (p != '.') {
        bool whitePiece = isWhitePiece(p);
        char type = whitePiece ? p : (p - 32);
        if (!whitePiece) {
          gfx.fillRect(x + 1, y + 1, 13, 13, 1);
          u8g2.setFontMode(1);
          u8g2.setForegroundColor(0);
        } else {
          gfx.fillRect(x + 1, y + 1, 13, 13, 0);
          if (isDarkSquare) gfx.drawRect(x + 1, y + 1, 13, 13, 1);
          u8g2.setFontMode(1);
          u8g2.setForegroundColor(1);
        }
        u8g2.setFont(u8g2_font_helvB10_tf);
        char buf[2] = {type, '\0'};
        int w = u8g2.getUTF8Width(buf);
        u8g2.setCursor(x + (15 - w) / 2, y + 12);
        u8g2.print(buf);
      } else {
        gfx.fillRect(x + 1, y + 1, 13, 13, 0);
        if (isDarkSquare) {
          gfx.drawRect(x + 1, y + 1, 13, 13, 1);
          gfx.drawPixel(x + 7, y + 7, 1);
        }
      }
    }
  }
  gfx.disableInversion = false;

  u8g2.setFont(BOLD_FONT);
  u8g2.setCursor(190, 20);
  u8g2.print("Daily Board");

  u8g2.setFont(MAIN_FONT);
  u8g2.setCursor(190, 40);
  u8g2.print("Rating:");
  u8g2.setCursor(190, 52);
  u8g2.print(g_chessPuzzleRating);

  u8g2.setCursor(190, 72);
  u8g2.print("Side:");
  u8g2.setCursor(190, 84);
  u8g2.print(g_chessPlayerIsWhite ? "White" : "Black");

  #if HAS_BATTERY
  updateBatteryCached(true);
  char batBuf[16];
  snprintf(batBuf, sizeof(batBuf), "%d%%", g_batValid ? g_batPctShown : 0);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(190, 114);
  u8g2.print(batBuf);
  #endif

  display.update();
}

void loop() {
  btns.poll();

  if (g_isrDropCount > BTN_QUEUE_RECOVER_THRESHOLD) {
    noInterrupts();
    g_isrDropCount = 0;
    interrupts();
    clearButtonQueue();
    btns.b1.resetState();
  }

  if (btns.b1.anyClick()) {
    markUserActivity();
  }

  bool charging = isUsbConnected();
  if (charging) {
    if (!g_servicesActive && mode != MODE_UPLOAD) {
      startUploadServicesOnly();
    }
  } else {
    if (g_servicesActive && mode != MODE_UPLOAD) {
      stopUploadServicesOnly();
    }
  }

  if (g_servicesActive && mode != MODE_UPLOAD) {
    server.handleClient();
  }

  if (ENABLE_DEEP_SLEEP && mode != MODE_UPLOAD) {
    if ((uint32_t)(millis() - lastUserActionMs) > SLEEP_AFTER_MS) {
      if (isUsbConnected()) {
        if (g_screensaverMode == 1 && g_spotifyClientId.length() > 0 && g_spotifyRefreshToken.length() > 0 && mode != MODE_SPOTIFY) {
          mode = MODE_SPOTIFY;
          g_spotifyForceRefresh = true;
          resetInputFrontend();
          return;
        }
      } else {
        goToSleep();
        return;
      }
    }
  }

  if (btns.b1.tripleClick && mode == MODE_UPLOAD) {
    stopUploadModeToLibrary();
    return;
  }

  // Universal triple click back handler
  if (btns.b1.tripleClick && mode != MODE_UPLOAD && mode != MODE_BM_PREVIEW) {
    if (mode == MODE_SPOTIFY) {
      exitSpotifyMode();
    } else if (mode == MODE_CHESS) {
      if (g_chessSubMode == CHESS_SUB_MOVE_SEL) {
        g_chessSubMode = CHESS_SUB_PIECE_SEL;
        drawChessScreen();
      } else {
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(50);
        esp_wifi_stop();
        enterLibraryRoot(true);
      }
    } else if (mode == MODE_LIBRARY) {
      if (currentLibraryFolder.length() > 0) {
        currentLibraryFolder = folderParent(currentLibraryFolder);
        selectedItem = 0;
        drawLibrary();
      }
    } else if (mode == MODE_SETTINGS) {
      mode = MODE_LIBRARY;
      drawLibrary();
    } else if (mode == MODE_ABOUT) {
      mode = MODE_SETTINGS;
      drawSettings();
    } else if (mode == MODE_TODO || mode == MODE_CALENDAR) {
      enterLibraryRoot(true);
    } else if (mode == MODE_CHAPTER_LIST || mode == MODE_BM_LIST) {
      mode = MODE_READER;
      g_jumpModeActive = true;
      renderCurrentPage();
    } else if (mode == MODE_BM_BOOK_SELECT) {
      mode = MODE_LIBRARY;
      drawLibrary();
    } else {
      enterLibraryRoot(true);
    }
    resetInputFrontend();
    return;
  }

  switch (mode) {
    case MODE_UPLOAD:
      handleModeUpload();
      break;

    case MODE_ABOUT:
      handleModeAbout();
      break;

    case MODE_BM_BOOK_SELECT:
      handleModeBookmarkBookSelect();
      break;

    case MODE_BM_LIST:
      handleModeBookmarkList();
      break;

    case MODE_BM_PREVIEW:
      handleModeBookmarkPreview();
      break;

    case MODE_LIBRARY:
      handleModeLibrary();
      break;

    case MODE_READER:
      handleModeReader();
      break;

    case MODE_SPOTIFY:
      handleModeSpotify();
      break;

    case MODE_SETTINGS:
      handleModeSettings();
      break;

    case MODE_CHESS:
      handleModeChess();
      break;

    case MODE_TODO:
      handleModeTodo();
      break;

    case MODE_CALENDAR:
      handleModeCalendar();
      break;

    case MODE_CHAPTER_LIST:
      handleModeChapterList();
      break;
  }
}

