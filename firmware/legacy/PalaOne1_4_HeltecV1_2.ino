#include <heltec-eink-modules.h>
EInkDisplay_WirelessPaperV1_2 display;

#include "pala_one_sleep_black_icon_v4.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <LittleFS.h>
#define FS LittleFS

#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
U8G2_FOR_ADAFRUIT_GFX u8g2;

#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_sleep.h>

// ---------------------- Firmware Version ----------------------
#define FW_VERSION "1.5.0"

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
#endif

// ---------------------- Button ----------------------
#define BTN 0

// “Feel” tuning
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
  MODE_BM_PREVIEW
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
  LIB_ENTRY_BACK,
  LIB_ENTRY_FOLDER,
  LIB_ENTRY_BOOK,
  LIB_ENTRY_BOOKMARKS,
  LIB_ENTRY_ABOUT,
  LIB_ENTRY_UPLOAD
};

static const int MAX_LIBRARY_ENTRIES = MAX_BOOKS + MAX_FOLDERS + 4;
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

// ---- Upload mode auto-exit ----
static uint32_t g_uploadStartMs           = 0;
static const uint32_t UPLOAD_AUTO_EXIT_MS = 15UL * 60UL * 1000UL;

// ============================================================================
//  GFX adapter
// ============================================================================
class HeltecGFXAdapter : public Adafruit_GFX {
public:
  HeltecGFXAdapter(EInkDisplay_WirelessPaperV1_2 &d) : Adafruit_GFX(W, H), disp(d) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint16_t c = (color ? BLACK : WHITE);
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

  invalidateMetrics();
}

// ============================================================================
//  ISR Button backend
// ============================================================================
static const uint8_t BTN_Q = 64;
static const uint32_t BTN_QUEUE_RECOVER_THRESHOLD = 10;

volatile uint8_t  btnQHead = 0;
volatile uint8_t  btnQTail = 0;
volatile bool     btnQState[BTN_Q];
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
  btnQState[btnQHead]  = (digitalRead(BTN) == LOW);
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

  void poll() {
    resetClicks();

    uint8_t headSnap;
    noInterrupts();
    headSnap = btnQHead;
    interrupts();

    while (btnQTail != headSnap) {
      noInterrupts();
      bool rawPressed = btnQState[btnQTail];
      uint32_t edgeT  = btnQTimeMs[btnQTail];
      btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q);
      interrupts();

      if ((uint32_t)(edgeT - lastStableChange) <= DEBOUNCE_MS) continue;
      if (rawPressed == stablePressed) continue;

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

    if (clickCount > 0) {
      uint32_t now = millis();
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
ButtonState btns;

// ---- Forward declarations ----
void drawBookmarksList();
void drawBookmarksBookSelect();
void drawLibrary();
void drawAbout();
void renderCurrentPage();
void startUploadMode();
void stopUploadModeToLibrary();
void goToSleep();
void drawSleepScreen();
void handleResetConfirm();
void handleResetDo();
void handleSettings();
void handleSettingsPost();
void handleUploadSleepDone();
void handleDeleteSleepImg();
void handleCreateFolder();
void handleMoveBook();
void handleDeleteFolder();

static void safeCloseBook();
static void enterLibraryRoot(bool redraw);
static void resetPreviewState();
static void resetUiEphemeralState();
static bool reopenCurrentBookIfNeeded();
static void syncWakeState(bool reading);
static void resetInputFrontend();
static void markUserActivity();

// ============================================================================
//  Helpers
// ============================================================================
bool fsBegin() { return FS.begin(false); }

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
  while (digitalRead(BTN) == LOW) delay(5);
  delay(DEBOUNCE_MS + 8);
  clearButtonQueue();
  btns.resetState();
  markUserActivity();
}

static bool reopenCurrentBookIfNeeded() {
  if (currentBookPath.length() == 0) return false;
  safeCloseBook();
  bookFile = FS.open(currentBookPath, "r");
  return (bool)bookFile;
}

void ensureBooksDir() {
  if (!FS.exists("/books")) FS.mkdir("/books");
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
    case LIB_ENTRY_BACK:      return ".. Back";
    case LIB_ENTRY_FOLDER:    return folderLeafLabel(String(folders[libraryEntryRefs[idx]]));
    case LIB_ENTRY_BOOK:      return bookLeafLabel(libraryEntryRefs[idx]);
    case LIB_ENTRY_BOOKMARKS: return "Bookmarks";
    case LIB_ENTRY_ABOUT:     return "About";
    case LIB_ENTRY_UPLOAD:    return "Upload";
  }
  return "";
}

static void buildLibraryEntries() {
  while (currentLibraryFolder.length() > 0 && !libraryFolderExists(currentLibraryFolder)) {
    currentLibraryFolder = folderParent(currentLibraryFolder);
  }

  libraryEntryCount = 0;

  if (currentLibraryFolder.length() > 0 && libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_BACK;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }

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
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_ABOUT;
    libraryEntryRefs[libraryEntryCount] = -1;
    libraryEntryCount++;
  }
  if (libraryEntryCount < MAX_LIBRARY_ENTRIES) {
    libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_UPLOAD;
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
static float    g_batVRaw = 0.0f;
static float    g_batVFiltered = 0.0f;
static int      g_batPct = 0;
static int      g_batPctShown = 0;
static bool     g_batValid = false;
static bool     g_batLow = false;
static uint32_t g_batLastMs = 0;
static const uint32_t BAT_CACHE_MS = 30000;

// Calibration factor for divider + ADC tolerance.
// Fine tune with a multimeter if needed, e.g. 1.018f or 0.987f.
static float g_batCalFactor = 1.00f;

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

  // Throw away first reads after enabling divider/ADC path.
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

  // Trim extremes, average the center values.
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

void updateBatteryCached(bool force = false) {
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
    // Smooth enough to avoid jumping, responsive enough for real usage.
    const float alpha = 0.22f;
    g_batVFiltered = (alpha * raw) + ((1.0f - alpha) * g_batVFiltered);
  }
  g_batVFiltered = clampf(g_batVFiltered, 3.0f, 4.25f);

  int pctRaw = batteryPercentFromOCV(g_batVFiltered);
  if (pctRaw < 0) pctRaw = 0;
  if (pctRaw > 100) pctRaw = 100;
  g_batPct = pctRaw;

  // Hysteresis for displayed percent so it feels product-like.
  if (force) {
    g_batPctShown = g_batPct;
  } else {
    if (g_batPct < g_batPctShown) {
      // Drop faster when battery is actually going down.
      if ((g_batPctShown - g_batPct) >= 1) g_batPctShown--;
    } else if (g_batPct > g_batPctShown + 2) {
      // Rise slower to avoid bounce after load changes.
      g_batPctShown++;
    }
  }

  if (g_batPctShown < 0) g_batPctShown = 0;
  if (g_batPctShown > 100) g_batPctShown = 100;

  // Low-battery hysteresis.
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
    // small low-battery notch inside icon for clearer warning
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

void drawCenter(const char* a, const char* b = nullptr) {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
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
  scanBooksRecursive("/books", "");
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

  knownPages = 1;
  pageOffsets[0] = 0;
  eofReached = false;

  pageIndex = prefs.getInt((currentBookKey + "_p").c_str(), 0);
  if (pageIndex < 0) pageIndex = 0;

  pageTurnsSinceFull = 0;
  resetSaveThrottle();
  syncWakeState(true);
  return true;
}

void drawStatusBar(uint32_t startOffset) {
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
    drawCenter("Open failed", "Returning to library");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  if (!bookFile || bookFile.isDirectory()) {
    drawCenter("Open failed", "Returning to library");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  size_t bookSize = bookFile.size();
  if (bookSize == 0) {
    drawCenter("Book empty", "Returning to library");
    delay(500);
    enterLibraryRoot(true);
    return;
  }

  ensureOffsetsUpTo(pageIndex);
  if (knownPages <= 0) {
    drawCenter("Book empty", "Returning to library");
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
  u8g2.print(title);

#if HAS_BATTERY
  drawBatteryTopRight();
#endif

  int lineY = yTitle + 4;
  gfx.drawFastHLine(MARGIN_X, lineY, W - (MARGIN_X * 2), 1);

  u8g2.setFont(MAIN_FONT);
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

  String subline = currentLibraryFolder.length() > 0 ? folderLeafLabel(currentLibraryFolder) : String("");
  int y = drawSectionHeader("Library", true);
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
  int y = drawSectionHeader("About", true);

  String rows[5];
  rows[0] = "1x next / down";
  rows[1] = "2x open / back";
  rows[2] = "3x home";
  rows[3] = "Hold bookmark";
  rows[4] = "TXT reader over Wi-Fi";

  for (int i = 0; i < 5; i++) {
    // tighter layout: use same baseline as other menus (no extra ascent offset)
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
//  Web UI
// ============================================================================
String webUiStyle() {
  return String(
    "<style>"
    "body{margin:0;background:#f3efe7;color:#1f2328;font:15px/1.45 system-ui,sans-serif}"
    ".wrap{max-width:760px;margin:0 auto;padding:18px}"
    ".wide{max-width:960px}"
    ".top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;margin-bottom:14px}"
    ".top a,.link{color:#3c5a7a;text-decoration:none}"
    ".muted{color:#667085;font-size:13px}"
    ".card{background:#fff;border:1px solid #ddd4c7;border-radius:14px;padding:14px 15px;margin:0 0 14px;box-shadow:0 1px 0 rgba(0,0,0,.03)}"
    ".grid{display:grid;gap:12px}"
    ".actions{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin-top:14px}"
    ".nav{display:flex;flex-wrap:wrap;gap:10px 14px;font-size:14px}"
    ".nav a{color:#3c5a7a;text-decoration:none}"
    ".list{list-style:none;padding:0;margin:0}"
    ".list li{padding:11px 0;border-top:1px solid #ece5d9}"
    ".list li:first-child{border-top:0;padding-top:0}"
    ".row{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}"
    ".meta{color:#667085;font-size:13px}"
    ".pill{display:inline-block;background:#f6f2ea;color:#6b6358;border-radius:999px;padding:3px 8px;font-size:12px}"
    ".pre{white-space:pre-wrap;line-height:1.4;padding:12px;border:1px solid #ddd4c7;border-radius:10px;background:#fcfaf7}"
    ".danger{background:#6e2a2a}"
    "button{border:0;border-radius:10px;background:#1f2328;color:#fff;padding:10px 14px;font:600 14px system-ui,sans-serif}"
    "input[type=text],input[type=file],select{width:100%;box-sizing:border-box;border:1px solid #c9c2b8;border-radius:10px;background:#fff;padding:10px;font:inherit}"
    ".stack{display:grid;gap:8px}"
    ".small{font-size:13px}"
    "h1,h2,h3,p{margin:0}"
    "h1,h2,h3{margin-bottom:6px}"
    "p + p{margin-top:10px}"
    "@media(max-width:640px){.row,.top{flex-direction:column}.wrap{padding:14px}}"
    "</style>"
  );
}

String webPageStart(const String& title, const String& subtitle, const String& navHtml, bool wide = false) {
  String out;
  out.reserve(900);
  out = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>";
  out += title;
  out += "</title>";
  out += webUiStyle();
  out += "</head><body><div class='wrap";
  if (wide) out += " wide";
  out += "'><div class='top'><div><h1>";
  out += title;
  out += "</h1><div class='muted'>";
  out += subtitle;
  out += "</div></div>";
  if (navHtml.length() > 0) {
    out += "<div class='nav'>";
    out += navHtml;
    out += "</div>";
  }
  out += "</div>";
  return out;
}

String webPageEnd() {
  return String("</div></body></html>");
}

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

void handleRoot() {
  loadBooks();
  char diagBuf[100];
  snprintf(diagBuf, sizeof(diagBuf), "Firmware %s &middot; %d books &middot; %.0f KB free",
           FW_VERSION, bookCount, (FS.totalBytes() - FS.usedBytes()) / 1024.0f);

  String out = webPageStart(
    "Pala One",
    String(diagBuf),
    "<a href='/files'>Files</a><a href='/bookmarks'>Bookmarks</a><a href='/settings'>Settings</a><a href='/reset'>Factory reset</a>"
  );

  out +=
    "<div class='card'><h2>Upload book</h2>"
    "<p class='muted'>Send plain text files to <b>/books</b> on the device, then sort them into folders from the Files page.</p>"
    "<form method='POST' action='/upload' enctype='multipart/form-data' accept-charset='UTF-8' style='margin-top:14px'>"
    "<input type='file' name='file' accept='.txt,text/plain' required>"
    "<div class='actions'><button type='submit'>Upload</button><span class='muted'>Use short press on the device to leave upload mode.</span></div>"
    "</form></div>";

  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
}

void handleFiles() {
  loadBooks();
  String out = webPageStart(
    "Files",
    "Manage books, folders and library structure for Pala One.",
    "<a href='/'>Home</a><a href='/bookmarks'>Bookmarks</a><a href='/settings'>Settings</a>"
  );

  out +=
    "<div class='card'><h2>Create folder</h2>"
    "<form method='POST' action='/mkdir' class='stack' accept-charset='UTF-8' style='margin-top:12px'>"
    "<input type='text' name='folder' placeholder='books or classics/english' maxlength='64'>"
    "<div class='actions'><button type='submit'>Create folder</button><span class='muted'>Folders live inside /books.</span></div>"
    "</form></div>";

  out += "<div class='card'><h2>Folders</h2>";
  if (folderCount == 0) {
    out += "<p class='muted'>No folders yet. Books currently live in the root of /books.</p>";
  } else {
    out += "<ul class='list'>";
    for (int i = 0; i < folderCount; i++) {
      out += "<li><div class='row'><div><span class='pill'>";
      out += htmlEscape(prettyRelativeLabel(String(folders[i])));
      out += "</span></div><div><a class='link' href='/rmdir?folder=";
      out += htmlEscape(folders[i]);
      out += "' onclick=\"return confirm('Delete folder? Only empty folders can be deleted.')\">Delete</a></div></div></li>";
    }
    out += "</ul>";
  }
  out += "</div>";

  out += "<div class='card'><h2>Library files</h2>";

  if (bookCount >= MAX_BOOKS) {
    out += "<p style='color:#b91c1c;font-weight:600'>&#9888; Library full (";
    out += String(MAX_BOOKS);
    out += " books max). Delete books to make room.</p>";
  }
  if (folderCount >= MAX_FOLDERS) {
    out += "<p style='color:#b91c1c;font-weight:600'>&#9888; Folder limit reached (";
    out += String(MAX_FOLDERS);
    out += " max).</p>";
  }

  if (bookCount == 0) {
    out += "<p class='muted'>No books uploaded yet.</p>";
  } else {
    out += "<ul class='list'>";
    for (int i = 0; i < bookCount; i++) {
      String folderLabel = books[i].folder[0] ? prettyRelativeLabel(books[i].folder) : String("Root");
      out += "<li><div class='row'><div><h3>";
      out += htmlEscape(String(books[i].name));
      out += "</h3><div class='meta'>";
      out += String((int)books[i].size);
      out += " bytes &middot; folder: ";
      out += htmlEscape(folderLabel);
      out += "</div><form method='POST' action='/move' class='stack small' accept-charset='UTF-8' style='margin-top:10px'>";
      out += "<input type='hidden' name='id' value='" + String(i) + "'>";
      out += "<input type='text' name='folder' value='";
      out += htmlEscape(String(books[i].folder));
      out += "' placeholder='leave blank for root' maxlength='64'>";
      out += "<div class='actions'><button type='submit'>Move</button><span class='muted'>Use the exact folder path.</span></div></form></div><div><a class='link' href='/del?id=" + String(i) + "' onclick=\"return confirm('Delete file?')\">Delete</a></div></div></li>";
    }
    out += "</ul>";
  }

  out += "</div>";
  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
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

  server.sendHeader("Location", "/files");
  server.send(302, "text/plain", "");
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

  server.sendHeader("Location", "/files");
  server.send(302, "text/plain", "");
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

  server.sendHeader("Location", "/files");
  server.send(302, "text/plain", "");
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
    server.sendHeader("Location", "/files");
    server.send(302, "text/plain", "");
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

  server.sendHeader("Location", "/files");
  server.send(302, "text/plain", "");
}

void handleUploadDone() {
  if (g_uploadOk) {
    loadBooks();
    String msg = "Upload OK: " + g_uploadFinalName + "\n";
    msg += String(bookCount) + " book(s) on device. ";
    msg += String((FS.totalBytes() - FS.usedBytes()) / 1024) + " KB free.\n";
    msg += "Short press on device exits upload mode.";
    server.send(200, "text/plain; charset=utf-8", msg);
  } else {
    server.send(400, "text/plain; charset=utf-8", g_uploadError.length() ? g_uploadError : "Upload failed");
  }
}

String readPageTextForWeb(const String &path, int page) {
  File f = FS.open(path, "r");
  if (!f) return String("Open failed.");
  uint32_t off = pageOffsetForPage(f, page);
  String out;
  out.reserve(900);
  (void)readPageFromFile(f, off, false, &out);
  f.close();
  out.trim();
  if (out.length() == 0) out = "(empty)";
  return out;
}

void handleBookmarksWeb() {
  loadBooks();
  String out = webPageStart(
    "Bookmarks",
    "Saved reading positions for Pala One, grouped by book.",
    "<a href='/'>Home</a><a href='/files'>Files</a><a href='/settings'>Settings</a>",
    true
  );

  if (bookCount == 0) {
    out += "<div class='card'><p class='muted'>No books available yet.</p></div>";
  }

  for (int i = 0; i < bookCount; i++) {
    String key = prefKeyForBook(String(books[i].path));
    uint16_t pages[MAX_BOOKMARKS];
    uint8_t count = loadBookmarksForKey(key, pages);

    out += "<div class='card'><h2>";
    out += htmlEscape(String(books[i].name));
    out += "</h2>";

    if (count == 0) {
      out += "<p class='muted'>No bookmarks</p></div>";
      continue;
    }

    File f = FS.open(String(books[i].path), "r");
    if (!f) {
      out += "<p class='muted'>Open failed</p></div>";
      continue;
    }

    out += "<ul class='list'>";
    uint32_t cursorOff = 0;
    int cursorPage = 0;

    for (int j = 0; j < count; j++) {
      int targetPage = (int)pages[j];
      if (targetPage < 0) targetPage = 0;

      while (cursorPage < targetPage) {
        uint32_t next = buildNextOffsetFor(f, cursorOff);
        if (next == cursorOff) break;
        cursorOff = next;
        cursorPage++;
      }

      String sn = readBookmarkLabelAtOffset(f, cursorOff, targetPage);

      out += "<li><div class='row'><div><div class='pill'>Bookmark ";
      out += String(j + 1);
      out += "</div><p class='meta' style='margin-top:8px'>";
      out += htmlEscape(sn);
      out += "</p></div><div><a class='link' href='/viewbm?book=" + String(i) + "&idx=" + String(j) + "'>View</a> | ";
      out += "<a class='link' href='/delbm?book=" + String(i) + "&idx=" + String(j) + "' onclick=\"return confirm('Delete bookmark?')\">Delete</a></div></div></li>";
    }

    out += "</ul></div>";
    f.close();
  }

  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
}

void handleDeleteBookmarkWeb() {
  if (!server.hasArg("book") || !server.hasArg("idx")) {
    server.send(400, "text/plain; charset=utf-8", "missing book/idx");
    return;
  }

  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if (b < 0 || b >= bookCount) {
    server.send(400, "text/plain; charset=utf-8", "bad book");
    return;
  }

  String key = prefKeyForBook(String(books[b].path));
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if (idx < 0 || idx >= count) {
    server.send(400, "text/plain; charset=utf-8", "bad idx");
    return;
  }

  for (int i = idx + 1; i < count; i++) pages[i - 1] = pages[i];
  count--;
  saveBookmarksForKey(key, pages, count);

  server.sendHeader("Location", "/bookmarks");
  server.send(302, "text/plain", "");
}

void handleViewBookmarkWeb() {
  if (!server.hasArg("book") || !server.hasArg("idx")) {
    server.send(400, "text/plain; charset=utf-8", "missing book/idx");
    return;
  }

  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if (b < 0 || b >= bookCount) {
    server.send(400, "text/plain; charset=utf-8", "bad book");
    return;
  }

  String key = prefKeyForBook(String(books[b].path));
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if (idx < 0 || idx >= count) {
    server.send(400, "text/plain; charset=utf-8", "bad idx");
    return;
  }

  int page = (int)pages[idx];
  String txt = readPageTextForWeb(String(books[b].path), page);

  String out = webPageStart(
    "Bookmark View",
    "Preview the saved page text for this bookmark.",
    "<a href='/bookmarks'>&#8592; Back</a><a href='/files'>Files</a><a href='/'>Home</a>",
    true
  );

  out += "<div class='card'><h2>";
  out += htmlEscape(String(books[b].name));
  out += "</h2>";

  out += "<p class='muted'>Bookmark ";
  out += String(idx + 1);
  out += "</p>";

  out += "<pre class='pre'>";
  out += htmlEscape(txt);
  out += "</pre></div>";

  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
}

void handleResetConfirm() {
  String out = webPageStart(
    "Factory Reset",
    "Erase all books, bookmarks, progress, and custom assets.",
    "<a href='/'>Back</a>"
  );
  out +=
    "<div class='card'><h2>Confirm reset</h2>"
    "<p><strong>This will delete ALL books, bookmarks and reading progress.</strong></p>"
    "<p class='muted'>The device filesystem will be formatted and settings will return to defaults.</p>"
    "<form method='POST' action='/reset' style='margin-top:14px'><button class='danger' type='submit'>Yes, reset</button></form>"
    "</div>";
  out += webPageEnd();

  server.send(200, "text/html; charset=utf-8", out);
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
  server.send(200, "text/plain; charset=utf-8",
              "Reset done. Reconnect and open http://192.168.4.1/ again.");
}

void handleSettings() {
  String sel8   = (g_fontSize == 8)    ? " selected" : "";
  String sel10  = (g_fontSize == 10)   ? " selected" : "";
  String sel12  = (g_fontSize == 12)   ? " selected" : "";
  String sel14  = (g_fontSize == 14)   ? " selected" : "";

  String ss30   = (g_sleepSecs == 30)   ? " selected" : "";
  String ss60   = (g_sleepSecs == 60)   ? " selected" : "";
  String ss120  = (g_sleepSecs == 120)  ? " selected" : "";
  String ss300  = (g_sleepSecs == 300)  ? " selected" : "";
  String ss600  = (g_sleepSecs == 600)  ? " selected" : "";
  String ss1800 = (g_sleepSecs == 1800) ? " selected" : "";

  String lg0 = (LINE_GAP == 0) ? " selected" : "";
  String lg1 = (LINE_GAP == 1) ? " selected" : "";
  String lg2 = (LINE_GAP == 2) ? " selected" : "";
  String lg3 = (LINE_GAP == 3) ? " selected" : "";

  String lpBm = (g_readerLongPressAction == LONGPRESS_BOOKMARK) ? " selected" : "";

  bool hasSleepImg = FS.exists("/sleep.bin");

  String out;
  out.reserve(4600);
  out =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Settings</title>"
    "<style>"
    "body{margin:0;background:#f3efe7;color:#1f2328;font:15px/1.45 system-ui,sans-serif}"
    ".wrap{max-width:760px;margin:0 auto;padding:18px}"
    ".top{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:14px}"
    ".top a,.link{color:#3c5a7a;text-decoration:none}"
    ".muted{color:#667085;font-size:13px}"
    ".card{background:#fff;border:1px solid #ddd4c7;border-radius:14px;padding:14px 15px;margin:0 0 14px;box-shadow:0 1px 0 rgba(0,0,0,.03)}"
    ".grid{display:grid;gap:12px}"
    "label{display:block;font-weight:600;margin:0 0 6px}"
    "select,input[type=file]{width:100%;box-sizing:border-box;border:1px solid #c9c2b8;border-radius:10px;background:#fff;padding:10px;font:inherit}"
    ".hint{margin:6px 0 0;color:#667085;font-size:12px}"
    ".actions{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin-top:14px}"
    "button{border:0;border-radius:10px;background:#1f2328;color:#fff;padding:10px 14px;font:600 14px system-ui,sans-serif}"
    ".status{padding:10px 12px;border-radius:10px;font-size:14px;margin:10px 0 0}"
    ".ok{background:#e7f6ec;color:#216e39}"
    ".idle{background:#f6f2ea;color:#6b6358}"
    "h1,h2{margin:0 0 6px}"
    "p{margin:0 0 10px}"
    "@media(min-width:620px){.grid.cols-2{grid-template-columns:1fr 1fr}}"
    "</style></head><body><div class='wrap'>"
    "<div class='top'><div><h1>Pala One Settings</h1><div class='muted'>Firmware 1.5 configuration page stored directly on the device.</div></div><a href='/'>&#8592; Home</a></div>"
    "<div class='card'><h2>Reading</h2><form method='POST' action='/settings' accept-charset='UTF-8'><div class='grid cols-2'><div><label for='font'>Font size</label><select id='font' name='font'>"
    "<option value='8'"; out += sel8; out += ">8px &mdash; tiny</option>";
  out += "<option value='10'"; out += sel10; out += ">10px &mdash; small</option>";
  out += "<option value='12'"; out += sel12; out += ">12px &mdash; medium</option>";
  out += "<option value='14'"; out += sel14; out += ">14px &mdash; large</option>";
  out +=
    "</select><div class='hint'>Controls how many lines fit on each page.</div></div>"
    "<div><label for='sleep'>Sleep after</label><select id='sleep' name='sleep'>"
    "<option value='30'"; out += ss30; out += ">30 seconds</option>";
  out += "<option value='60'"; out += ss60; out += ">1 minute</option>";
  out += "<option value='120'"; out += ss120; out += ">2 minutes</option>";
  out += "<option value='300'"; out += ss300; out += ">5 minutes</option>";
  out += "<option value='600'"; out += ss600; out += ">10 minutes</option>";
  out += "<option value='1800'"; out += ss1800; out += ">30 minutes</option>";
  out += "</select><div class='hint'>Auto-sleep keeps battery draw low while idle.</div></div>";
  out += "<div><label for='lgap'>Line spacing</label><select id='lgap' name='lgap'>";
  out += "<option value='0'"; out += lg0; out += ">0 px &mdash; compact</option>";
  out += "<option value='1'"; out += lg1; out += ">1 px &mdash; normal</option>";
  out += "<option value='2'"; out += lg2; out += ">2 px &mdash; relaxed</option>";
  out += "<option value='3'"; out += lg3; out += ">3 px &mdash; loose</option>";
  out += "</select><div class='hint'>A small change here can make text much easier to scan.</div></div>";

  out += "";
out +=
    "</div>"
    "<div class='actions' style='margin-top:24px;'><button type='submit'>Save settings</button><span class='muted'>No extra files, scripts, or fonts.</span></div></form></div>"
    "<div class='card'><h2>Screensaver</h2>"
    "<p>Upload raw XBM bytes: <b>3904 bytes</b>, 250&times;122 px, 1-bit, LSB-first, 32 bytes per row.</p>"
    "<p class='muted'>Tip: use <a class='link' href='https://javl.github.io/image2cpp/' target='_blank'>image2cpp</a> with <b>Plain bytes</b>. Invert colors if needed.</p>";

  if (hasSleepImg) {
    out += "<div class='status ok'>&#10003; Custom screensaver active. "
           "<a class='link' href='/del-sleep' onclick=\"return confirm('Delete custom screensaver?')\">Delete</a></div>";
  } else {
    out += "<div class='status idle'>Using built-in screensaver.</div>";
  }

  out +=
    "<form method='POST' action='/upload-sleep' enctype='multipart/form-data' style='margin-top:14px'>"
    "<div class='grid'><div><label for='file'>Sleep image file</label><input id='file' type='file' name='file' accept='.bin'></div></div>"
    "<div class='actions'><button type='submit'>Upload image</button></div>"
    "</form></div></div></body></html>";

  server.send(200, "text/html; charset=utf-8", out);
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
  if (server.hasArg("lpact")) {
    g_readerLongPressAction = LONGPRESS_BOOKMARK;
    prefs.putInt("cfg_lpact", LONGPRESS_BOOKMARK);
  }

  server.sendHeader("Location", "/settings");
  server.send(302, "text/plain", "");
}

void handleDeleteSleepImg() {
  if (FS.exists("/sleep.bin")) FS.remove("/sleep.bin");
  server.sendHeader("Location", "/settings");
  server.send(302, "text/plain", "");
}

void handleUploadSleepDone() {
  if (g_sleepUploadOk) {
    server.sendHeader("Location", "/settings");
    server.send(302, "text/plain", "");
  } else {
    server.send(400, "text/plain; charset=utf-8", g_sleepUploadError.length() ? g_sleepUploadError : "Sleep image upload failed");
  }
}

// ============================================================================
//  Upload mode start/stop
// ============================================================================
void startUploadMode() {
  mode = MODE_UPLOAD;
  g_uploadStartMs = millis();

  prepareMenuFrame();
  u8g2.setFont(BOLD_FONT);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  String url = String("http://") + ip.toString();

  u8g2.setCursor(MARGIN_X, 11);
  u8g2.print("Upload Mode");
  gfx.drawFastHLine(MARGIN_X, 16, W - (MARGIN_X * 2), 1);

  u8g2.setFont(MAIN_FONT);
  int y = 33;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("Wi-Fi"); y += 12;
  u8g2.setCursor(MARGIN_X + 8, y); u8g2.print(AP_SSID); y += 14;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("Password"); y += 12;
  u8g2.setCursor(MARGIN_X + 8, y); u8g2.print(AP_PASS); y += 14;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(url.c_str()); y += 16;

  u8g2.setFont(MAIN_FONT);
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("1x click to exit");

  display.update();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/del", HTTP_GET, handleDelete);
  server.on("/mkdir", HTTP_POST, handleCreateFolder);
  server.on("/move", HTTP_POST, handleMoveBook);
  server.on("/rmdir", HTTP_GET, handleDeleteFolder);

  server.on("/reset", HTTP_GET, handleResetConfirm);
  server.on("/reset", HTTP_POST, handleResetDo);

  server.on("/bookmarks", HTTP_GET, handleBookmarksWeb);
  server.on("/delbm", HTTP_GET, handleDeleteBookmarkWeb);
  server.on("/viewbm", HTTP_GET, handleViewBookmarkWeb);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsPost);
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

      String clean = sanitizeUploadedFilename(up.filename);
      g_uploadFinalName = clean;
      String finalPath = "/books/" + clean;
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

  server.begin();
}

void stopUploadModeToLibrary() {
  server.stop();

  if (uploadFile) uploadFile.close();
  if (sleepUploadFile) sleepUploadFile.close();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();
  btStop();

  uploadPending = "";
  uploadPath = "";
  g_uploadOk = false;
  g_uploadError = "";
  g_uploadFinalName = "";

  g_sleepUploadOk = false;
  g_sleepUploadError = "";
  g_sleepUploadTmpPath = "";

  loadBooks();
  mode = MODE_LIBRARY;
  resetInputFrontend();
  drawLibrary();
}

void drawSleepScreen() {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();

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

  drawSleepScreen();
  delay(600);

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  esp_wifi_stop();
  btStop();

  Platform::prepareToSleep();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN, 0);

  delay(50);
  esp_deep_sleep_start();
}

static inline void idleLightSleepMaybe() {
  if (mode == MODE_UPLOAD) return;
  if (!ENABLE_DEEP_SLEEP) return;
  if (toastUntilMs != 0 && (int32_t)(millis() - toastUntilMs) <= 0) return;
  if (btnQTail != btnQHead) return;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN, 0);
  esp_light_sleep_start();

  while (digitalRead(BTN) == LOW) delay(5);
  delay(DEBOUNCE_MS + 5);
}

// ============================================================================
//  Setup / Mode handlers / Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Keep full CPU speed. Underclocking made menu + reader input feel sluggish
// because page render, pagination and display updates block the main loop longer.
setCpuFrequencyMhz(240);

  pinMode(BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN), btnISR, CHANGE);

  u8g2.begin(gfx);

  invalidateMetrics();
  (void)getMetrics();

#if HAS_BATTERY
  adcSetupOnce();
  pinMode(BAT_ADC_CTRL, INPUT);
  updateBatteryCached(true);
#endif

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
  if (prefs.getInt("wake_mode", 0) == 1) {
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
    drawLibrary();
    resetInputFrontend();
  }
}

static void handleModeUpload() {
  server.handleClient();
  bool timeout = (uint32_t)(millis() - g_uploadStartMs) > UPLOAD_AUTO_EXIT_MS;
  if (btns.shortClick || timeout) stopUploadModeToLibrary();
}

static void handleModeAbout() {
  if (btns.shortClick || btns.doubleClick || btns.longClick || btns.quadClick) {
    mode = MODE_LIBRARY;
    drawLibrary();
  }
}

static void handleModeBookmarkBookSelect() {
  if (bookCount == 0) {
    if (btns.anyClick()) {
      mode = MODE_LIBRARY;
      drawLibrary();
    }
    return;
  }

  if (btns.shortClick) {
    bmBookIndex++;
    if (bmBookIndex >= bookCount) bmBookIndex = 0;
    drawBookmarksBookSelect();
    return;
  }

  if (btns.longClick) {
    bmBookIndex--;
    if (bmBookIndex < 0) bmBookIndex = bookCount - 1;
    drawBookmarksBookSelect();
    return;
  }

  if (btns.doubleClick) {
    bmSelIndex = 0;
    mode = MODE_BM_LIST;
    drawBookmarksList();
  }
}

static void handleModeBookmarkList() {
  String key = prefKeyForBook(String(books[bmBookIndex].path));
  bmCount = loadBookmarksForKey(key, bmPages);
  if (bmSelIndex >= (int)bmCount) bmSelIndex = max(0, (int)bmCount - 1);

  if (btns.shortClick) {
    if (bmCount > 0) {
      bmSelIndex++;
      if (bmSelIndex >= (int)bmCount) bmSelIndex = 0;
    }
    drawBookmarksList();
    return;
  }

  if (btns.doubleClick) {
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

  if (btns.longClick) {
    mode = MODE_BM_BOOK_SELECT;
    drawBookmarksBookSelect();
    return;
  }
}

static void handleModeBookmarkPreview() {
  // 3x = Vorschau verwerfen, echte Leseposition wiederherstellen
  if (btns.tripleClick) {
    bmPreviewActive = false;
    pageIndex = bmPreviewSavedPage;
    mode = MODE_READER;
    renderCurrentPage();
    return;
  }

  // Hold = ab dieser Bookmark-Page weiterlesen
  if (btns.longClick) {
    bmPreviewActive = false;
    saveProgressThrottled(true);
    mode = MODE_READER;
    renderCurrentPage();
    return;
  }

  if (btns.doubleClick) {
    pageIndex--;
    if (pageIndex < 0) pageIndex = 0;
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.shortClick) {
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

  if (btns.shortClick) {
    selectedItem++;
    if (selectedItem >= totalItems) selectedItem = 0;
    drawLibrary();
    return;
  }

  if (!btns.doubleClick) return;

  if (selectedItem < 0 || selectedItem >= libraryEntryCount) {
    drawLibrary();
    return;
  }

  LibraryEntryType entryType = libraryEntryTypes[selectedItem];
  int entryRef = libraryEntryRefs[selectedItem];

  if (entryType == LIB_ENTRY_BACK) {
    currentLibraryFolder = folderParent(currentLibraryFolder);
    selectedItem = 0;
    drawLibrary();
    return;
  }

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

  if (entryType == LIB_ENTRY_ABOUT) {
    mode = MODE_ABOUT;
    drawAbout();
    return;
  }

  startUploadMode();
}

static void handleModeReader() {
  if (btns.longClick) {
    const char* msg = addBookmarkForCurrentBook();
    if (msg) showToast(msg);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.doubleClick) {
    if (pageIndex > 0) pageIndex--;
    saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  if (btns.shortClick) {
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

  // Reader input must stay extremely responsive, so no experimental idle behavior here.
}

void loop() {
  btns.poll();

  if (g_isrDropCount > BTN_QUEUE_RECOVER_THRESHOLD) {
    noInterrupts();
    g_isrDropCount = 0;
    interrupts();
    clearButtonQueue();
    btns.resetState();
  }

  if (btns.anyClick()) {
    markUserActivity();
  }

  if (ENABLE_DEEP_SLEEP && mode != MODE_UPLOAD) {
    if ((uint32_t)(millis() - lastUserActionMs) > SLEEP_AFTER_MS) {
      goToSleep();
      return;
    }
  }

  if (btns.tripleClick && mode == MODE_UPLOAD) {
    stopUploadModeToLibrary();
    return;
  }

  if (btns.tripleClick && mode != MODE_UPLOAD && mode != MODE_BM_PREVIEW) {
    enterLibraryRoot(true);
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
  }
}
