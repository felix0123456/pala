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

// ---------------------- Device / Display ----------------------
static const int W = 250;
static const int H = 122;

// ---------------------- Bookmarks ----------------------
static const uint8_t MAX_BOOKMARKS = 12;

// ---------------------- Battery ----------------------
#define HAS_BATTERY 1
#if HAS_BATTERY
  #define BAT_ADC_CTRL 19   // enable divider (active LOW)
  #define BAT_ADC_IN   20   // ADC input
#endif

// ---------------------- Button ----------------------
#define BTN 0 // GPIO0 BOOT, active LOW

// “Feel” tuning (very snappy)
static const uint32_t DOUBLE_MS   = 330;
static const uint32_t TRIPLE_MS   = 520;
static const uint32_t QUAD_MS     = 760;
static const uint32_t LONG_MS     = 700;
static const uint32_t VERYLONG_MS = 2100;
static const uint32_t DEBOUNCE_MS = 35;

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
  LONGPRESS_BOOKMARK = 0,
  LONGPRESS_PAGE1    = 1
};
static int g_readerLongPressAction = LONGPRESS_BOOKMARK;

// ---------------------- WiFi Upload AP ----------------------
static const char* AP_SSID = "EBOOK-READER";
static const char* AP_PASS = "12345678";
WebServer server(80);
File uploadFile;
File sleepUploadFile;
String uploadPath;

// ---------------------- Preferences / FS ----------------------
Preferences prefs;

// ---------------------- UI ----------------------
static const int MARGIN_X = 6;
static const int TOP_PAD  = 0;
static const int BOT_PAD  = 0;

static const bool SHOW_PROGRESS_BAR = true;
static const bool SHOW_PAGE_NUMBER  = true;

static const uint8_t* PAGE_FONT = u8g2_font_5x8_tf;

const uint8_t* MAIN_FONT = u8g2_font_helvR08_te;
const uint8_t* BOLD_FONT = u8g2_font_helvB08_te;

static const int STATUS_H = 8;

int LINE_GAP = 0;

static const int FULL_REFRESH_EVERY_N_PAGES = 30;
static const int MENU_FULL_REFRESH_EVERY   = 20;

// Power
static const bool ENABLE_DEEP_SLEEP = true;
uint32_t SLEEP_AFTER_MS = 120 * 1000;

// ---------------------- Books / Library ----------------------
struct BookInfo {
  String name;
  String path;
  size_t size;
  String folder;
};

static const int MAX_BOOKS = 80;
BookInfo books[MAX_BOOKS];
int bookCount = 0;

static const int MAX_FOLDERS = 32;
String folders[MAX_FOLDERS];
int folderCount = 0;

// Library: 0..books-1, books=Bookmarks, books+1=About, books+2=Upload
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
static const int MAX_PAGES = 12000;
uint32_t pageOffsets[MAX_PAGES];
int knownPages = 0;
bool eofReached = false;

// Bookmarks UI state
int bmBookIndex = 0;
int bmSelIndex  = 0;
uint16_t bmPages[MAX_BOOKMARKS];
uint8_t bmCount = 0;

bool bmPreviewActive = false;
int savedProgressPage = 0;
int bmPreviewBookIndex = -1;

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
    case  8: MAIN_FONT = u8g2_font_helvR08_te; BOLD_FONT = u8g2_font_helvB08_te; break;
    case 10: MAIN_FONT = u8g2_font_helvR10_te; BOLD_FONT = u8g2_font_helvB10_te; break;
    case 12: MAIN_FONT = u8g2_font_helvR12_te; BOLD_FONT = u8g2_font_helvB12_te; break;
    case 14: MAIN_FONT = u8g2_font_helvR14_te; BOLD_FONT = u8g2_font_helvB14_te; break;
    default: MAIN_FONT = u8g2_font_helvR10_te; BOLD_FONT = u8g2_font_helvB10_te; sz = 10; break;
  }
  g_fontSize = sz;
  invalidateMetrics();
}

static void loadSettings() {
  int fs = prefs.getInt("cfg_font", 8);
  applyFontSize(fs);

  g_sleepSecs = (uint32_t)prefs.getInt("cfg_sleep", 120);
  if (g_sleepSecs <   10) g_sleepSecs =   10;
  if (g_sleepSecs > 3600) g_sleepSecs = 3600;
  SLEEP_AFTER_MS = g_sleepSecs * 1000UL;

  LINE_GAP = prefs.getInt("cfg_lgap", 0);
  if (LINE_GAP < 0) LINE_GAP = 0;
  if (LINE_GAP > 4) LINE_GAP = 4;

  int saved = prefs.getInt("cfg_lpact", -1);

  if (saved == -1) {
    g_readerLongPressAction = LONGPRESS_BOOKMARK; // echter Default
  } else {
    g_readerLongPressAction = saved;
  }

  if (g_readerLongPressAction != LONGPRESS_BOOKMARK && g_readerLongPressAction != LONGPRESS_PAGE1) {
    g_readerLongPressAction = LONGPRESS_BOOKMARK;
  }

  invalidateMetrics();
}

// ============================================================================
//  ISR Button backend
// ============================================================================
static const uint8_t BTN_Q = 16;

volatile uint8_t  btnQHead = 0;
volatile uint8_t  btnQTail = 0;
volatile bool     btnQState[BTN_Q];
volatile uint32_t btnQTimeMs[BTN_Q];

static inline uint32_t isrNowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void IRAM_ATTR btnISR() {
  uint8_t next = (uint8_t)((btnQHead + 1) % BTN_Q);
  if (next == btnQTail) {
    btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q);
  }
  btnQState[btnQHead] = digitalRead(BTN);
  btnQTimeMs[btnQHead] = isrNowMs();
  btnQHead = next;
}

// ============================================================================
//  Button handler
// ============================================================================
struct ButtonState {
  bool stableState = HIGH;
  uint32_t lastStableChange = 0;

  uint32_t pressStart = 0;
  uint32_t lastRelease = 0;

  uint8_t clickCount = 0;
  uint32_t firstClickRelease = 0;

  bool shortClick=false;
  bool doubleClick=false;
  bool tripleClick=false;
  bool quadClick=false;
  bool longClick=false;
  bool veryLongClick=false;

  void resetClicks(){
    shortClick=false; doubleClick=false; tripleClick=false; quadClick=false; longClick=false; veryLongClick=false;
  }

  void poll() {
    resetClicks();

    while (btnQTail != btnQHead) {
      noInterrupts();
      bool raw = btnQState[btnQTail];
      uint32_t edgeT = btnQTimeMs[btnQTail];
      btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q);
      interrupts();

      if ((uint32_t)(edgeT - lastStableChange) <= DEBOUNCE_MS) continue;

      bool prev = stableState;
      stableState = raw;
      lastStableChange = edgeT;

      if (prev == HIGH && stableState == LOW) {
        pressStart = edgeT;
      }

      if (prev == LOW && stableState == HIGH) {
        uint32_t dur = (uint32_t)(edgeT - pressStart);

        if (dur >= VERYLONG_MS) {
          clickCount = 0;
          veryLongClick = true;
        } else if (dur >= LONG_MS) {
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
    }

    if (clickCount > 0) {
      uint32_t now = millis();

      bool quadExpired = (uint32_t)(now - firstClickRelease) > QUAD_MS;
      bool doubleExpired = (uint32_t)(now - lastRelease) > DOUBLE_MS;

      bool emit = false;
      if (clickCount <= 2) emit = doubleExpired;
      else if (clickCount == 3) emit = quadExpired;

      if (emit) {
        if (clickCount == 1) shortClick = true;
        else if (clickCount == 2) doubleClick = true;
        else if (clickCount == 3) tripleClick = true;
        clickCount = 0;
      }
    }
  }

  bool anyClick() const {
    return shortClick || doubleClick || tripleClick || quadClick || longClick || veryLongClick;
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
void factoryReset();
void handleResetConfirm();
void handleResetDo();
void handleSettings();
void handleSettingsPost();
void handleUploadSleepDone();
void handleDeleteSleepImg();
void handleCreateFolder();
void handleMoveBook();
void handleDeleteFolder();

// ============================================================================
//  Helpers
// ============================================================================
bool fsBegin() { return FS.begin(true); }
void ensureBooksDir() { if (!FS.exists("/books")) FS.mkdir("/books"); }

static String stripTxtExt(const String& s){
  if (s.endsWith(".txt")) return s.substring(0, s.length()-4);
  return s;
}
static String safePathFromDirEntry(const String& name){
  if (name.startsWith("/")) return name;
  return String("/books/") + name;
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
static String relativeBookPath(const String& path) {
  if (path.startsWith("/books/")) return path.substring(7);
  if (path == "/books") return "";
  if (path.startsWith("/books")) return path.substring(6);
  return path;
}
static String folderFromBookPath(const String& path) {
  String rel = relativeBookPath(path);
  int slash = rel.lastIndexOf('/');
  if (slash < 0) return "";
  return rel.substring(0, slash);
}

// Keep UTF-8 bytes >= 128 intact, so äöüß survive.
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
    String part = (slash >= 0) ? normalized.substring(start, slash) : normalized.substring(start);
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
        c >= 128)
    {
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
    if (folders[i] == folderRel) return;
  }
  if (folderCount < MAX_FOLDERS) folders[folderCount++] = folderRel;
}
static void sortFolders() {
  for (int i = 0; i < folderCount - 1; i++) {
    for (int j = i + 1; j < folderCount; j++) {
      if (folders[j] < folders[i]) {
        String tmp = folders[i];
        folders[i] = folders[j];
        folders[j] = tmp;
      }
    }
  }
}
static void sortBooks() {
  for (int i = 0; i < bookCount - 1; i++) {
    for (int j = i + 1; j < bookCount; j++) {
      if (books[j].name < books[i].name) {
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
  String leaf = stripTxtExt(lastPathComponent(books[idx].path));
  leaf.replace('_', ' ');
  return leaf;
}
static bool libraryFolderExists(const String& folderRel) {
  if (folderRel.length() == 0) return true;
  for (int i = 0; i < folderCount; i++) {
    if (folders[i] == folderRel) return true;
  }
  for (int i = 0; i < bookCount; i++) {
    if (books[i].folder == folderRel) return true;
  }
  return false;
}
static String libraryEntryLabel(int idx) {
  if (idx < 0 || idx >= libraryEntryCount) return "";
  switch (libraryEntryTypes[idx]) {
    case LIB_ENTRY_BACK:      return ".. Back";
    case LIB_ENTRY_FOLDER:    return folderLeafLabel(folders[libraryEntryRefs[idx]]);
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
    if (folderParent(folders[i]) == currentLibraryFolder) {
      libraryEntryTypes[libraryEntryCount] = LIB_ENTRY_FOLDER;
      libraryEntryRefs[libraryEntryCount] = i;
      libraryEntryCount++;
    }
  }

  for (int i = 0; i < bookCount && libraryEntryCount < MAX_LIBRARY_ENTRIES; i++) {
    if (books[i].folder == currentLibraryFolder) {
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
      books[bookCount].path = absPath;
      books[bookCount].folder = relDir;
      books[bookCount].name = prettyRelativeLabel(relFile);
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
  while (*s) { h ^= (uint8_t)(*s++); h *= 16777619u; }
  return h;
}

String prefKeyForBook(const String& path) {
  uint32_t h = fnv1a32(path.c_str());
  char buf[16];
  snprintf(buf, sizeof(buf), "b_%08lx", (unsigned long)h);
  return String(buf);
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
  for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 128) return false;
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

void saveProgressThrottled(bool force=false) {
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
void saveProgress() { saveProgressThrottled(true); }

// ============================================================================
//  Battery
// ============================================================================
#if HAS_BATTERY
static float g_batV = 0.0f;
static int   g_batPct = 0;
static uint32_t g_batLastMs = 0;
static const uint32_t BAT_CACHE_MS = 60000;

static inline void adcSetupOnce() {
  pinMode(BAT_ADC_IN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_IN, ADC_11db);
}

static inline uint32_t readAdcMilliVoltsAveraged() {
  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, LOW);
  delay(8);

  (void)analogReadMilliVolts(BAT_ADC_IN);
  delay(2);

  uint32_t sum = 0;
  for (int i=0;i<10;i++){
    sum += analogReadMilliVolts(BAT_ADC_IN);
    delay(2);
  }

  pinMode(BAT_ADC_CTRL, INPUT);
  return sum / 10;
}

float readBatteryVoltage() {
  uint32_t mv = readAdcMilliVoltsAveraged();
  return (mv / 1000.0f) * 2.0f;
}

int batteryPercentFromVoltage(float v) {
  if (v >= 4.20f) return 100;
  if (v <= 3.30f) return 0;
  if (v >= 3.70f) return (int)(55 + (v - 3.70f) * (45.0f / 0.50f));
  if (v >= 3.50f) return (int)(20 + (v - 3.50f) * (35.0f / 0.20f));
  return (int)(0 + (v - 3.30f) * (20.0f / 0.20f));
}

void updateBatteryCached(bool force=false){
  uint32_t now = millis();
  if(!force && (now - g_batLastMs) < BAT_CACHE_MS) return;
  g_batLastMs = now;

  g_batV = readBatteryVoltage();
  bool valid = (g_batV > 2.5f && g_batV < 4.6f);
  if (valid) {
    g_batPct = batteryPercentFromVoltage(g_batV);
    if (g_batPct < 0) g_batPct = 0;
    if (g_batPct > 100) g_batPct = 100;
  }
}

static void drawBatteryTopRight() {
  updateBatteryCached(false);
  bool valid = (g_batV > 2.5f && g_batV < 4.6f);
  int pct = valid ? g_batPct : 0;
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

  u8g2.setFont(u8g2_font_6x10_tf);
  char buf[8];
  if (valid) snprintf(buf, sizeof(buf), "%d%%", pct);
  else       snprintf(buf, sizeof(buf), "--");
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
void beginPageCanvas(bool clearMem=true) {
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
  if ((int32_t)(millis() - toastUntilMs) > 0) { toastUntilMs = 0; toastMsg = ""; return; }

  u8g2.setFont(u8g2_font_6x10_tf);
  int y = H - STATUS_H - 2;
  int x = MARGIN_X;
  u8g2.setCursor(x, y);
  u8g2.print(toastMsg.c_str());
  u8g2.setFont(MAIN_FONT);
}

void drawCenter(const char* a, const char* b=nullptr) {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH = (ascent - descent) + LINE_GAP;

  int y = (H/2) - lineH/2;
  if (b) y -= lineH/2;

  int wA = u8g2.getUTF8Width(a);
  u8g2.setCursor((W - wA)/2, y);
  u8g2.print(a);

  if (b) {
    y += lineH;
    int wB = u8g2.getUTF8Width(b);
    u8g2.setCursor((W - wB)/2, y);
    u8g2.print(b);
  }
  display.update();
}

static void showModalBottomMessage(const char* msg, uint16_t ms = 1200) {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();

  u8g2.setFont(u8g2_font_6x10_tf);
  int y = H - 2;
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(msg);

  display.update();
  delay(ms);

  u8g2.setFont(MAIN_FONT);
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

  auto flushLine = [&](const String& toPrint){
    if (draw) {
      u8g2.setCursor(MARGIN_X, cursorY);
      u8g2.print(toPrint.c_str());
      cursorY += m.lineH;
    }
    if (outText) {
      String t = toPrint; t.trim();
      (*outText) += t; (*outText) += "\n";
    }
    linesUsed++;
  };

  auto safeReturn = [&](uint32_t off) -> uint32_t {
    if (off <= startPos) off = startPos + 1;
    size_t sz = f.size();
    if (sz > 0 && off > sz) off = sz;
    return off;
  };

  auto hardBreakWord = [&](String &w, uint32_t &wStartPos){
    while (w.length() > 0) {
      String chunk = ""; chunk.reserve(32);
      for (int i = 0; i < (int)w.length(); i++) {
        chunk += w[i];
        if (u8g2.getUTF8Width(chunk.c_str()) > m.maxWidth) {
          chunk.remove(chunk.length() - 1);
          break;
        }
      }
      if (chunk.length() == 0) chunk = w.substring(0, 1);

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
    if (word.length() > 0)      next = wordStartPos;
    else if (line.length() > 0) next = lineStartPos;
    else                        next = f.position();
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
    if (next <= start) { eofReached = true; break; }
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
  if (bookFile) bookFile.close();
  if (idx < 0 || idx >= bookCount) return false;

  String path = books[idx].path;
  bookFile = FS.open(path, "r");
  if (!bookFile) return false;

  currentBookKey  = prefKeyForBook(path);
  currentBookPath = path;

  knownPages = 1;
  pageOffsets[0] = 0;
  eofReached = false;

  pageIndex = prefs.getInt((currentBookKey + "_p").c_str(), 0);
  if (pageIndex < 0) pageIndex = 0;

  pageTurnsSinceFull = 0;
  resetSaveThrottle();
  return true;
}

void drawStatusBar(uint32_t startOffset) {
  const int y0 = H - STATUS_H;

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
  ensureOffsetsUpTo(pageIndex);

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

  const int PREFETCH_AHEAD = 3;
  if (!eofReached && knownPages < MAX_PAGES) {
    if (pageIndex == knownPages - 1) {
      if (nextOff <= start || nextOff >= bookFile.size()) eofReached = true;
      else pageOffsets[knownPages++] = nextOff;
    }
    while (!eofReached && knownPages < MAX_PAGES && knownPages <= (pageIndex + PREFETCH_AHEAD)) {
      uint32_t s = pageOffsets[knownPages - 1];
      uint32_t n = buildNextOffset(s);
      if (n <= s || n >= bookFile.size()) { eofReached = true; break; }
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
static String bmKeyFor(const String& bookKey){ return bookKey + "_bm"; }

uint8_t loadBookmarksForKey(const String& bookKey, uint16_t outPages[MAX_BOOKMARKS]) {
  uint8_t buf[1 + MAX_BOOKMARKS * 2] = {0};
  size_t got = prefs.getBytes(bmKeyFor(bookKey).c_str(), buf, sizeof(buf));
  if (got < 1) return 0;

  uint8_t count = buf[0];
  if (count > MAX_BOOKMARKS) count = MAX_BOOKMARKS;

  for (uint8_t i=0;i<count;i++){
    uint16_t lo = buf[1 + i*2 + 0];
    uint16_t hi = buf[1 + i*2 + 1];
    outPages[i] = (uint16_t)((hi<<8) | lo);
  }
  return count;
}

void saveBookmarksForKey(const String& bookKey, const uint16_t pages[MAX_BOOKMARKS], uint8_t count) {
  if (count > MAX_BOOKMARKS) count = MAX_BOOKMARKS;

  uint8_t buf[1 + MAX_BOOKMARKS * 2] = {0};
  buf[0] = count;
  for (uint8_t i=0;i<count;i++){
    buf[1 + i*2 + 0] = (uint8_t)(pages[i] & 0xFF);
    buf[1 + i*2 + 1] = (uint8_t)((pages[i] >> 8) & 0xFF);
  }
  prefs.putBytes(bmKeyFor(bookKey).c_str(), buf, sizeof(buf));
}

const char* addBookmarkForCurrentBook() {
  if (currentBookKey.length() == 0) return nullptr;

  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(currentBookKey, pages);

  for (uint8_t i=0;i<count;i++){
    if ((int)pages[i] == pageIndex) {
      return "Bookmark exists";
    }
  }

  if (count < MAX_BOOKMARKS) pages[count++] = (uint16_t)pageIndex;
  else {
    for (uint8_t i=1;i<MAX_BOOKMARKS;i++) pages[i-1] = pages[i];
    pages[MAX_BOOKMARKS-1] = (uint16_t)pageIndex;
    count = MAX_BOOKMARKS;
  }

  for (uint8_t i=0;i<count;i++){
    for (uint8_t j=i+1;j<count;j++){
      if (pages[j] < pages[i]) { uint16_t t=pages[i]; pages[i]=pages[j]; pages[j]=t; }
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
void drawLibrary() {
  prepareMenuFrame();
  buildLibraryEntries();

  const uint8_t* FONT_NORMAL = MAIN_FONT;
  const uint8_t* FONT_BOLD   = BOLD_FONT;

  u8g2.setFont(FONT_NORMAL);

  int ascent  = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH   = (ascent - descent) + LINE_GAP;

  int y = ascent + 4;

  u8g2.setCursor(MARGIN_X, y);
  if (currentLibraryFolder.length() == 0) u8g2.print("Library");
  else u8g2.print(folderLeafLabel(currentLibraryFolder).c_str());
#if HAS_BATTERY
  drawBatteryTopRight();
#endif
  y += lineH + 4;

  if (currentLibraryFolder.length() > 0) {
    u8g2.setFont(FONT_NORMAL);
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Folder");
    y += lineH;
  }

  int totalItems = libraryEntryCount;

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 3) visible = 3;
  if (visible > 7) visible = 7;

  int top = selectedItem - (visible / 2);
  if (top < 0) top = 0;
  if (top > totalItems - visible) top = max(0, totalItems - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= totalItems) break;

    u8g2.setFont(FONT_NORMAL);
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(idx == selectedItem ? "> " : "  ");

    if (libraryEntryTypes[idx] == LIB_ENTRY_BOOK) {
      u8g2.setFont(FONT_BOLD);
    } else {
      u8g2.setFont(FONT_NORMAL);
    }
    String label = libraryEntryLabel(idx);
    if (libraryEntryTypes[idx] == LIB_ENTRY_FOLDER) {
      label = String("[") + label + "]";
    }
    u8g2.print(label.c_str());

    y += lineH;
  }

  display.update();
}

void drawAbout() {
  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  int ascent = u8g2.getFontAscent();
  int lineH = (ascent - u8g2.getFontDescent()) + LINE_GAP;
  int y = ascent + 4;

  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("E-Book Reader");
#if HAS_BATTERY
  drawBatteryTopRight();
#endif
  y += lineH + 6;

  u8g2.setCursor(MARGIN_X, y); u8g2.print("1x → next / down"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("2x → open / back"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("3x → Home");        y += lineH;

  if (g_readerLongPressAction == LONGPRESS_BOOKMARK) {
  u8g2.setCursor(MARGIN_X, y); u8g2.print("Hold → bookmark"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("4x → Page 1");     y += lineH;
} else {
  u8g2.setCursor(MARGIN_X, y); u8g2.print("Hold → Page 1");   y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("4x → bookmark");   y += lineH;
}

  display.update();
}

void drawBookmarksBookSelect() {
  prepareMenuFrame();

  const uint8_t* FONT_NORMAL = MAIN_FONT;
  const uint8_t* FONT_BOLD   = BOLD_FONT;

  u8g2.setFont(FONT_NORMAL);

  int ascent  = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH   = (ascent - descent) + LINE_GAP;

  int y = ascent + 4;

  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("Bookmarks");
#if HAS_BATTERY
  drawBatteryTopRight();
#endif
  y += lineH + 4;

  if (bookCount == 0) {
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("No books");
    display.update();
    return;
  }

  if (bmBookIndex < 0) bmBookIndex = 0;
  if (bmBookIndex >= bookCount) bmBookIndex = bookCount - 1;

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 2) visible = 2;
  if (visible > 7) visible = 7;

  int top = bmBookIndex - (visible / 2);
  if (top < 0) top = 0;
  if (top > bookCount - visible) top = max(0, bookCount - visible);

  for (int i = 0; i < visible; i++) {
    int idx = top + i;
    if (idx >= bookCount) break;

    u8g2.setFont(FONT_NORMAL);
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(idx == bmBookIndex ? "> " : "  ");

    u8g2.setFont(idx == bmBookIndex ? FONT_BOLD : FONT_NORMAL);
    u8g2.print(books[idx].name.c_str());

    y += lineH;
  }

  display.update();
}

void drawBookmarksList() {
  prepareMenuFrame();

  u8g2.setFont(MAIN_FONT);
  int ascentTitle = u8g2.getFontAscent();
  int lineHTitle  = (ascentTitle - u8g2.getFontDescent()) + 2;

  int y = ascentTitle + 4;
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(books[bmBookIndex].name.c_str());

#if HAS_BATTERY
  drawBatteryTopRight();
#endif

  y += lineHTitle + 6;

  u8g2.setFont(MAIN_FONT);
  int ascent = u8g2.getFontAscent();
  int lineH  = (ascent - u8g2.getFontDescent()) + LINE_GAP;

  String key = prefKeyForBook(books[bmBookIndex].path);
  bmCount = loadBookmarksForKey(key, bmPages);
  if (bmSelIndex >= (int)bmCount) bmSelIndex = max(0, (int)bmCount - 1);

  if (bmCount == 0) {
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("No bookmarks");
    display.update();
    return;
  }

  File f = FS.open(books[bmBookIndex].path, "r");
  if (!f) {
    u8g2.setCursor(MARGIN_X, y);
    u8g2.print("Open failed");
    display.update();
    return;
  }

  int visible = (H - y - BOT_PAD) / lineH;
  if (visible < 1) visible = 1;
  if (visible > 6) visible = 6;

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

  for (int i=0;i<visible;i++) {
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

    u8g2.setCursor(MARGIN_X, y);
    u8g2.print(idx == bmSelIndex ? "> " : "  ");
    u8g2.print(sn.c_str());
    y += lineH;
  }

  f.close();
  display.update();
}

// ============================================================================
//  Upload web UI
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
    "</style>");
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

void handleRoot(){
  String out = webPageStart(
    "EBOOK-READER",
    "Upload books and manage the reader from a tiny built-in web portal.",
    "<a href='/files'>Files</a><a href='/bookmarks'>Bookmarks</a><a href='/settings'>Settings</a><a href='/reset'>Factory reset</a>");

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

void handleFiles(){
  loadBooks();
  String out = webPageStart(
    "Files",
    "Books currently stored on the device. Create folders like sci-fi or books/classics and move books into them.",
    "<a href='/'>Home</a><a href='/bookmarks'>Bookmarks</a><a href='/settings'>Settings</a>");

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
      out += htmlEscape(prettyRelativeLabel(folders[i]));
      out += "</span></div><div><a class='link' href='/rmdir?folder=";
      out += htmlEscape(folders[i]);
      out += "' onclick=\"return confirm('Delete folder? Only empty folders can be deleted.')\">Delete</a></div></div></li>";
    }
    out += "</ul>";
  }
  out += "</div>";

  out += "<div class='card'><h2>Library files</h2>";

  if (bookCount == 0) {
    out += "<p class='muted'>No books uploaded yet.</p>";
  } else {
    out += "<ul class='list'>";
    for(int i=0;i<bookCount;i++){
      String folderLabel = books[i].folder.length() ? prettyRelativeLabel(books[i].folder) : String("Root");
      out += "<li><div class='row'><div><h3>";
      out += htmlEscape(books[i].name);
      out += "</h3><div class='meta'>";
      out += String((int)books[i].size);
      out += " bytes &middot; folder: ";
      out += htmlEscape(folderLabel);
      out += "</div><form method='POST' action='/move' class='stack small' accept-charset='UTF-8' style='margin-top:10px'>";
      out += "<input type='hidden' name='id' value='" + String(i) + "'>";
      out += "<input type='text' name='folder' value='";
      out += htmlEscape(books[i].folder);
      out += "' placeholder='leave blank for root' maxlength='64'>";
      out += "<div class='actions'><button type='submit'>Move</button><span class='muted'>Use the exact folder path.</span></div></form></div><div><a class='link' href='/del?id=" + String(i) + "' onclick=\"return confirm('Delete file?')\">Delete</a></div></div></li>";
    }
    out += "</ul>";
  }

  out += "</div>";
  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
}

void handleDelete(){
  if (bookFile) bookFile.close();

  if(!server.hasArg("id")) { server.send(400,"text/plain; charset=utf-8","missing id"); return; }
  int id = server.arg("id").toInt();
  if(id < 0 || id >= bookCount) { server.send(400,"text/plain; charset=utf-8","bad id"); return; }

  String path = books[id].path;
  if (FS.exists(path)) FS.remove(path);

  server.sendHeader("Location","/files");
  server.send(302,"text/plain","");
}

void handleCreateFolder(){
  ensureBooksDir();
  if(!server.hasArg("folder")) { server.send(400, "text/plain; charset=utf-8", "missing folder"); return; }

  String folder = sanitizeFolderInput(server.arg("folder"));
  if (folder.length() == 0) { server.send(400, "text/plain; charset=utf-8", "bad folder"); return; }

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

void handleMoveBook(){
  loadBooks();
  if(!server.hasArg("id")) { server.send(400, "text/plain; charset=utf-8", "missing id"); return; }

  int id = server.arg("id").toInt();
  if(id < 0 || id >= bookCount) { server.send(400, "text/plain; charset=utf-8", "bad id"); return; }

  String oldPath = books[id].path;
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

  if (bookFile && currentBookPath == oldPath) bookFile.close();
  if (!FS.rename(oldPath, newPath)) {
    server.send(500, "text/plain; charset=utf-8", "move failed");
    return;
  }

  migrateBookMetadata(oldPath, newPath);
  server.sendHeader("Location", "/files");
  server.send(302, "text/plain", "");
}

void handleUploadDone(){
  server.send(200, "text/plain; charset=utf-8", "Upload OK.\nShort press on device exits upload mode.");
}

String readPageTextForWeb(const String &path, int page) {
  File f = FS.open(path, "r");
  if (!f) return String("Open failed.");
  uint32_t off = pageOffsetForPage(f, page);
  String out; out.reserve(900);
  (void)readPageFromFile(f, off, false, &out);
  f.close();
  out.trim();
  if (out.length() == 0) out = "(empty)";
  return out;
}

void handleBookmarksWeb(){
  loadBooks();
  String out = webPageStart(
    "Bookmarks",
    "Saved reading positions grouped by book.",
    "<a href='/'>Home</a><a href='/files'>Files</a><a href='/settings'>Settings</a>",
    true);

  if (bookCount == 0) {
    out += "<div class='card'><p class='muted'>No books available yet.</p></div>";
  }

  for(int i=0;i<bookCount;i++){
    String key = prefKeyForBook(books[i].path);
    uint16_t pages[MAX_BOOKMARKS];
    uint8_t count = loadBookmarksForKey(key, pages);

    out += "<div class='card'><h2>";
    out += htmlEscape(books[i].name);
    out += "</h2>";

    if(count==0){
      out += "<p class='muted'>No bookmarks</p></div>";
      continue;
    }

    File f = FS.open(books[i].path, "r");
    if (!f) { out += "<p class='muted'>Open failed</p></div>"; continue; }

    out += "<ul class='list'>";
    uint32_t cursorOff  = 0;
    int      cursorPage = 0;

    for(int j=0;j<count;j++){
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
      out += "<a class='link' href='/delbm?book=" + String(i) + "&idx=" + String(j) +
             "' onclick=\"return confirm('Delete bookmark?')\">Delete</a></div></div></li>";
    }

    out += "</ul></div>";
    f.close();
  }

  out += webPageEnd();
  server.send(200, "text/html; charset=utf-8", out);
}

void handleDeleteBookmarkWeb(){
  if(!server.hasArg("book") || !server.hasArg("idx")) { server.send(400,"text/plain; charset=utf-8","missing book/idx"); return; }
  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if(b<0 || b>=bookCount) { server.send(400,"text/plain; charset=utf-8","bad book"); return; }

  String key = prefKeyForBook(books[b].path);
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if(idx<0 || idx>=count) { server.send(400,"text/plain; charset=utf-8","bad idx"); return; }

  for(int i=idx+1;i<count;i++) pages[i-1] = pages[i];
  count--;
  saveBookmarksForKey(key, pages, count);

  server.sendHeader("Location","/bookmarks");
  server.send(302,"text/plain","");
}

void handleViewBookmarkWeb() {
  if(!server.hasArg("book") || !server.hasArg("idx")) { server.send(400,"text/plain; charset=utf-8","missing book/idx"); return; }
  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if(b<0 || b>=bookCount) { server.send(400,"text/plain; charset=utf-8","bad book"); return; }

  String key = prefKeyForBook(books[b].path);
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if(idx<0 || idx>=count) { server.send(400,"text/plain; charset=utf-8","bad idx"); return; }

  int page = (int)pages[idx];
  String txt = readPageTextForWeb(books[b].path, page);

  String out = webPageStart(
    "Bookmark View",
    "Preview the saved page text for this bookmark.",
    "<a href='/bookmarks'>&#8592; Back</a><a href='/files'>Files</a><a href='/'>Home</a>",
    true);

  out += "<div class='card'><h2>";
  out += htmlEscape(books[b].name);
  out += "</h2>";

  out += "<p class='muted'>Bookmark ";
  out += String(idx+1);
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
    "<a href='/'>Back</a>");
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
  if (bookFile) bookFile.close();

  prefs.clear();

  FS.end();
  delay(100);

  FS.format();
  delay(200);

  if (!FS.begin(true)) {
    return;
  }

  ensureBooksDir();
  loadBooks();
  selectedItem = 0;
}

void handleResetDo() {
  doFactoryReset();
  server.send(200, "text/plain; charset=utf-8",
              "Reset done. Reconnect and open http://192.168.4.1/ again.");
}

// ============================================================================
//  Settings web UI
// ============================================================================
void handleSettings() {
  String sel8   = (g_fontSize ==  8) ? " selected" : "";
  String sel10  = (g_fontSize == 10) ? " selected" : "";
  String sel12  = (g_fontSize == 12) ? " selected" : "";
  String sel14  = (g_fontSize == 14) ? " selected" : "";

  String ss30   = (g_sleepSecs ==   30) ? " selected" : "";
  String ss60   = (g_sleepSecs ==   60) ? " selected" : "";
  String ss120  = (g_sleepSecs ==  120) ? " selected" : "";
  String ss300  = (g_sleepSecs ==  300) ? " selected" : "";
  String ss600  = (g_sleepSecs ==  600) ? " selected" : "";
  String ss1800 = (g_sleepSecs == 1800) ? " selected" : "";

  String lg0 = (LINE_GAP == 0) ? " selected" : "";
  String lg1 = (LINE_GAP == 1) ? " selected" : "";
  String lg2 = (LINE_GAP == 2) ? " selected" : "";
  String lg3 = (LINE_GAP == 3) ? " selected" : "";

  String lpBm = (g_readerLongPressAction == LONGPRESS_BOOKMARK) ? " selected" : "";
  String lpP1 = (g_readerLongPressAction == LONGPRESS_PAGE1)    ? " selected" : "";

  bool hasSleepImg = FS.exists("/sleep.bin");

  String out;
  out.reserve(4200);
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
    "<div class='top'><div><h1>Settings</h1><div class='muted'>Lightweight config page stored in firmware only.</div></div><a href='/'>&#8592; Home</a></div>"
    "<div class='card'><h2>Reading</h2><form method='POST' action='/settings' accept-charset='UTF-8'><div class='grid cols-2'><div><label for='font'>Font size</label><select id='font' name='font'>"
    "<option value='8'";
  out += sel8;  out += ">8px &mdash; tiny</option>";
  out += "<option value='10'"; out += sel10; out += ">10px &mdash; small</option>";
  out += "<option value='12'"; out += sel12; out += ">12px &mdash; medium</option>";
  out += "<option value='14'"; out += sel14; out += ">14px &mdash; large</option>";
  out +=
    "</select><div class='hint'>Controls how many lines fit on each page.</div></div>"
    "<div><label for='sleep'>Sleep after</label><select id='sleep' name='sleep'>"
    "<option value='30'";
  out += ss30;   out += ">30 seconds</option>";
  out += "<option value='60'";   out += ss60;   out += ">1 minute</option>";
  out += "<option value='120'";  out += ss120;  out += ">2 minutes</option>";
  out += "<option value='300'";  out += ss300;  out += ">5 minutes</option>";
  out += "<option value='600'";  out += ss600;  out += ">10 minutes</option>";
  out += "<option value='1800'"; out += ss1800; out += ">30 minutes</option>";
  out += "</select><div class='hint'>Auto-sleep keeps battery draw low while idle.</div></div>";
  out += "<div><label for='lgap'>Line spacing</label><select id='lgap' name='lgap'>";
  out += "<option value='0'"; out += lg0; out += ">0 px &mdash; compact</option>";
  out += "<option value='1'"; out += lg1; out += ">1 px &mdash; normal</option>";
  out += "<option value='2'"; out += lg2; out += ">2 px &mdash; relaxed</option>";
  out += "<option value='3'"; out += lg3; out += ">3 px &mdash; loose</option>";
  out += "</select><div class='hint'>A small change here can make text much easier to scan.</div></div>";

  out += "<div><label for='lpact'>Long press in reader</label><select id='lpact' name='lpact'>";
  out += "<option value='0'"; out += lpBm; out += ">Save bookmark</option>";
  out += "<option value='1'"; out += lpP1; out += ">Jump to page 1</option>";
  out += "</select><div class='hint'>4x press performs the opposite action.</div>";
  out +=
    "</div>"
    "<div class='actions'><button type='submit'>Save settings</button><span class='muted'>No extra files, scripts, or fonts.</span></div></form></div>"
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
    if (ss <   10) ss =   10;
    if (ss > 3600) ss = 3600;
    g_sleepSecs    = (uint32_t)ss;
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
    int act = server.arg("lpact").toInt();
    if (act != LONGPRESS_BOOKMARK && act != LONGPRESS_PAGE1) act = LONGPRESS_BOOKMARK;
    g_readerLongPressAction = act;
    prefs.putInt("cfg_lpact", act);
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
  server.sendHeader("Location", "/settings");
  server.send(302, "text/plain", "");
}

// ============================================================================
//  Upload mode start/stop
// ============================================================================
void startUploadMode(){
  mode = MODE_UPLOAD;
  g_uploadStartMs = millis();

  prepareMenuFrame();
  u8g2.setFont(MAIN_FONT);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  String url = String("http://") + ip.toString();

  int ascent=u8g2.getFontAscent(), descent=u8g2.getFontDescent();
  int lineH=(ascent - descent) + LINE_GAP;
  int y = TOP_PAD + ascent;

  u8g2.setCursor(MARGIN_X, y); u8g2.print("UPLOAD MODE");     y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(AP_SSID);           y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(AP_PASS);           y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(url.c_str());       y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("1x click: exit");

  display.update();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/del", HTTP_GET, handleDelete);
  server.on("/mkdir", HTTP_POST, handleCreateFolder);
  server.on("/move", HTTP_POST, handleMoveBook);
  server.on("/rmdir", HTTP_GET, handleDeleteFolder);

  server.on("/reset", HTTP_GET,  handleResetConfirm);
  server.on("/reset", HTTP_POST, handleResetDo);

  server.on("/bookmarks", HTTP_GET, handleBookmarksWeb);
  server.on("/delbm", HTTP_GET, handleDeleteBookmarkWeb);
  server.on("/viewbm", HTTP_GET, handleViewBookmarkWeb);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/del-sleep", HTTP_GET, handleDeleteSleepImg);
  server.on("/upload-sleep", HTTP_POST, handleUploadSleepDone, [](){
    HTTPUpload& upS = server.upload();
    if (upS.status == UPLOAD_FILE_START) {
      if (FS.exists("/sleep.bin")) FS.remove("/sleep.bin");
      sleepUploadFile = FS.open("/sleep.bin", "w");
    } else if (upS.status == UPLOAD_FILE_WRITE) {
      if (sleepUploadFile) sleepUploadFile.write(upS.buf, upS.currentSize);
    } else if (upS.status == UPLOAD_FILE_END) {
      if (sleepUploadFile) sleepUploadFile.close();
    }
  });

  server.on("/upload", HTTP_POST, handleUploadDone, [](){
    HTTPUpload& up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
      String clean = sanitizeUploadedFilename(up.filename);
      uploadPath = "/books/" + clean;

      if (FS.exists(uploadPath)) FS.remove(uploadPath);
      uploadFile = FS.open(uploadPath, "w");
    }
    else if (up.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) uploadFile.write(up.buf, up.currentSize);
    }
    else if (up.status == UPLOAD_FILE_END) {
      if (uploadFile) uploadFile.close();
    }
  });

  server.begin();
}

void stopUploadModeToLibrary(){
  server.stop();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();

  loadBooks();
  mode = MODE_LIBRARY;
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
void goToSleep(){
  if (!ENABLE_DEEP_SLEEP) return;

  if (mode == MODE_READER && !bmPreviewActive) saveProgressThrottled(true);

  bool wasReading = (mode == MODE_READER || mode == MODE_BM_PREVIEW)
                    && currentBookPath.length() > 0;
  prefs.putInt("wake_mode", wasReading ? 1 : 0);
  if (wasReading) prefs.putString("wake_path", currentBookPath);

  if (bookFile) bookFile.close();

  drawSleepScreen();
  delay(600);

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
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
}

// ============================================================================
//  Setup / Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  setCpuFrequencyMhz(80);

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

  if (!fsBegin()) { drawCenter("FS mount failed"); return; }
  ensureBooksDir();

  prefs.begin("ereader", false);
  loadSettings();
  loadBooks();

  lastUserActionMs = millis();

  bool restored = false;
  if (prefs.getInt("wake_mode", 0) == 1) {
    String wp = prefs.getString("wake_path", "");
    if (wp.length() > 0) {
      for (int i = 0; i < bookCount; i++) {
        if (books[i].path == wp) {
          if (openBookByIndex(i)) {
            bmPreviewActive    = false;
            mode               = MODE_READER;
            pageTurnsSinceFull = FULL_REFRESH_EVERY_N_PAGES;
            renderCurrentPage();
            restored = true;
          }
          break;
        }
      }
    }
  }
  if (!restored) drawLibrary();
}

void loop() {
  btns.poll();
  if (btns.anyClick()) lastUserActionMs = millis();

  if (ENABLE_DEEP_SLEEP && mode != MODE_UPLOAD) {
    if ((uint32_t)(millis() - lastUserActionMs) > SLEEP_AFTER_MS) {
      goToSleep();
      return;
    }
  }

  if (btns.tripleClick) {
    if (mode == MODE_UPLOAD) {
      currentLibraryFolder = "";
      stopUploadModeToLibrary();
    } else {
      if (bookFile) bookFile.close();
      bmPreviewActive = false;
      currentLibraryFolder = "";
      mode = MODE_LIBRARY;
      drawLibrary();
    }
    return;
  }

  // UPLOAD
  if (mode == MODE_UPLOAD) {
    server.handleClient();
    bool timeout = (uint32_t)(millis() - g_uploadStartMs) > UPLOAD_AUTO_EXIT_MS;
    if (btns.shortClick || timeout) { stopUploadModeToLibrary(); return; }
    return;
  }

  // ABOUT
  if (mode == MODE_ABOUT) {
    if (btns.anyClick()) { mode = MODE_LIBRARY; drawLibrary(); }
    return;
  }

  // BOOKMARKS: select book
  if (mode == MODE_BM_BOOK_SELECT) {
    if (bookCount == 0) {
      if (btns.anyClick()) { mode = MODE_LIBRARY; drawLibrary(); }
      return;
    }

    if (btns.shortClick) {
      bmBookIndex++; if (bmBookIndex >= bookCount) bmBookIndex = 0;
      drawBookmarksBookSelect(); return;
    }
    if (btns.longClick) {
      bmBookIndex--; if (bmBookIndex < 0) bmBookIndex = bookCount - 1;
      drawBookmarksBookSelect(); return;
    }
    if (btns.doubleClick) {
      bmSelIndex = 0;
      mode = MODE_BM_LIST;
      drawBookmarksList();
      return;
    }
    return;
  }

  // BOOKMARKS: list
  if (mode == MODE_BM_LIST) {
    String key = prefKeyForBook(books[bmBookIndex].path);
    bmCount = loadBookmarksForKey(key, bmPages);
    if (bmSelIndex >= (int)bmCount) bmSelIndex = max(0, (int)bmCount-1);

    if (btns.shortClick) {
      if (bmCount > 0) { bmSelIndex++; if (bmSelIndex >= (int)bmCount) bmSelIndex = 0; }
      drawBookmarksList(); return;
    }

    if (btns.doubleClick) {
      if (bmCount == 0) return;

      if (openBookByIndex(bmBookIndex)) {
        savedProgressPage = prefs.getInt((currentBookKey + "_p").c_str(), 0);
        bmPreviewActive = true;
        bmPreviewBookIndex = bmBookIndex;

        pageIndex = (int)bmPages[bmSelIndex];
        if (pageIndex < 0) pageIndex = 0;
        mode = MODE_BM_PREVIEW;
        renderCurrentPage();
      } else {
        mode = MODE_LIBRARY; drawLibrary();
      }
      return;
    }

    if (btns.longClick) { mode = MODE_BM_BOOK_SELECT; drawBookmarksBookSelect(); return; }
    return;
  }

  // BOOKMARK PREVIEW MODE
  if (mode == MODE_BM_PREVIEW) {
    if (btns.longClick) {
      bmPreviewActive = false;
      saveProgressThrottled(true);
      mode = MODE_READER;
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

    if (btns.doubleClick) {
      pageIndex--;
      if (pageIndex < 0) pageIndex = 0;
      pageTurnsSinceFull++;
      renderCurrentPage();
      return;
    }
    return;
  }

  // LIBRARY
  if (mode == MODE_LIBRARY) {
    buildLibraryEntries();
    int totalItems = libraryEntryCount;

    if (btns.shortClick) {
      selectedItem++;
      if (selectedItem >= totalItems) selectedItem = 0;
      drawLibrary();
      return;
    }

    if (btns.doubleClick) {
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
        currentLibraryFolder = folders[entryRef];
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
      return;
    }
    return;
  }

 // READER
if (mode == MODE_READER) {

  // 4x = opposite action
  if (btns.quadClick) {
    if (g_readerLongPressAction == LONGPRESS_BOOKMARK) {
      pageIndex = 0;
      showToast("Page 1");
      saveProgressThrottled(true);
    } else {
      const char* msg = addBookmarkForCurrentBook();
      if (msg) showToast(msg);
    }

    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  // HOLD = primary action
  if (btns.longClick || btns.veryLongClick) {
    if (g_readerLongPressAction == LONGPRESS_BOOKMARK) {
      const char* msg = addBookmarkForCurrentBook();
      if (msg) showToast(msg);
    } else {
      pageIndex = 0;
      showToast("Page 1");
      saveProgressThrottled(true);
    }

    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  // NEXT
  if (btns.shortClick) {
    pageIndex++;
    ensureOffsetsUpTo(pageIndex);

    if (eofReached && pageIndex >= knownPages)
      pageIndex = knownPages - 1;

    saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  // BACK
  if (btns.doubleClick) {
    pageIndex--;
    if (pageIndex < 0) pageIndex = 0;

    saveProgressThrottled(false);
    pageTurnsSinceFull++;
    renderCurrentPage();
    return;
  }

  return;
}

if (!btns.anyClick() && mode == MODE_READER) {
  idleLightSleepMaybe();
}
}