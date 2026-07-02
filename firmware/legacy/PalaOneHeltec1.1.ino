#include <heltec-eink-modules.h>
EInkDisplay_WirelessPaperV1_1 display;

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
static const uint32_t DOUBLE_MS   = 330;   // tighter = snappier
static const uint32_t TRIPLE_MS   = 520;   // 3 clicks within this window (from first release)
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

// ---------------------- WiFi Upload AP ----------------------
static const char* AP_SSID = "EBOOK-READER";
static const char* AP_PASS = "12345678";
WebServer server(80);
File uploadFile;
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

static const int STATUS_H = 8;      // vorher 12

static const int LINE_GAP = 0;      // vorher 1

static const int FULL_REFRESH_EVERY_N_PAGES = 30;
static const int MENU_FULL_REFRESH_EVERY   = 20;

// Power
static const bool ENABLE_DEEP_SLEEP = true;
static const uint32_t SLEEP_AFTER_MS = 30 * 1000;

// ---------------------- Books / Library ----------------------
struct BookInfo {
  String name;
  String path;
  size_t size;
};

static const int MAX_BOOKS = 80;
BookInfo books[MAX_BOOKS];
int bookCount = 0;

// Library: 0..books-1, books=Bookmarks, books+1=About, books+2=Upload
int selectedItem = 0;

// Reader state
File bookFile;
String currentBookKey;
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

// Toast (non-blocking feedback)
String toastMsg;
uint32_t toastUntilMs = 0;
static const uint32_t TOAST_MS = 650;

// ============================================================================
//  FIX: correct mapping (Map 3 = 180°)
// ============================================================================
class HeltecGFXAdapter : public Adafruit_GFX {
public:
  HeltecGFXAdapter(EInkDisplay_WirelessPaperV1_1 &d) : Adafruit_GFX(W, H), disp(d) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint16_t c = (color ? BLACK : WHITE);
    int16_t xx = (W - 1) - x;
    int16_t yy = (H - 1) - y;
    disp.drawPixel(xx, yy, c);
  }
private:
  EInkDisplay_WirelessPaperV1_1 &disp;
};
HeltecGFXAdapter gfx(display);

// ============================================================================
//  Layout metrics cache  (IMPORTANT: defined BEFORE getMetrics())
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
    u8g2.setFont(u8g2_font_7x14_tf);

    g_metrics.ascent  = u8g2.getFontAscent();
    g_metrics.descent = u8g2.getFontDescent();
    g_metrics.lineH   = (g_metrics.ascent - g_metrics.descent) + LINE_GAP;

    // FULL width for text
    int w = W - (MARGIN_X * 2);
    if (w < 50) w = 50;
    g_metrics.maxWidth = w;

    // Reserve fixed bottom status bar
    int maxHeight = H - TOP_PAD - BOT_PAD;
    if (SHOW_PROGRESS_BAR || SHOW_PAGE_NUMBER) maxHeight -= STATUS_H;

    g_metrics.maxLines = maxHeight / g_metrics.lineH;
    if (g_metrics.maxLines < 1) g_metrics.maxLines = 1;

    g_metricsValid = true;
  }
  return g_metrics;
}

// ============================================================================
//  ISR Button backend (queued edges = lossless)
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
    btnQTail = (uint8_t)((btnQTail + 1) % BTN_Q); // drop oldest
  }
  btnQState[btnQHead] = digitalRead(BTN);
  btnQTimeMs[btnQHead] = isrNowMs();
  btnQHead = next;
}

// ============================================================================
//  Button handler (ISR-based, lossless)
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
  bool longClick=false;
  bool veryLongClick=false;

  void resetClicks(){
    shortClick=false; doubleClick=false; tripleClick=false; longClick=false; veryLongClick=false;
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

          if (clickCount >= 3) {
            clickCount = 0;
            tripleClick = true;
          }
        }
      }
    }

    if (clickCount > 0) {
      uint32_t now = millis();

      bool tripleExpired = (uint32_t)(now - firstClickRelease) > TRIPLE_MS;
      bool doubleExpired = (uint32_t)(now - lastRelease) > DOUBLE_MS;

      if (tripleExpired || doubleExpired) {
        if (clickCount == 1) shortClick = true;
        else                 doubleClick = true;
        clickCount = 0;
      }
    }
  }

  bool anyClick() const {
    return shortClick || doubleClick || tripleClick || longClick || veryLongClick;
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

static String readSnippetAtOffset(File &f, uint32_t off) {
  if (!f.seek(off)) return "Seek failed";
  String s; s.reserve(64);
  while (f.available() && s.length() < 60) {
    char c = (char)f.read();
    if (c == '\r') continue;
    if (c == '\n') break;
    if (c == '\t') c = ' ';
    s += c;
  }
  s.trim();
  if (s.length() > 55) s = s.substring(0, 55) + "...";
  if (s.length() == 0) s = "(empty)";
  return s;
}

static inline bool isAsciiOnly(const String& s) {
  for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 128) return false;
  return true;
} 

// ============================================================================
//  Progress saving (throttled)
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
//  Battery (cached)
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

  pinMode(BAT_ADC_CTRL, INPUT); // divider OFF
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

  u8g2.setFont(u8g2_font_7x14_tf);
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
  int y = H - STATUS_H + 9;      // inside status bar
  int x = MARGIN_X;
  u8g2.setCursor(x, y);
  u8g2.print(toastMsg.c_str());
  u8g2.setFont(u8g2_font_7x14_tf);
}

void drawCenter(const char* a, const char* b=nullptr) {
  display.fastmodeOff();
  display.clear();
  beginPageCanvas();
  u8g2.setFont(u8g2_font_7x14_tf);

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
  int y = H - 2;                 // ganz unten
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(msg);

  display.update();
  delay(ms);

  u8g2.setFont(u8g2_font_7x14_tf);
}
// ============================================================================
//  Text layout core (SAFE; UTF-8 safe hard-break only ASCII tokens)
// ============================================================================
uint32_t readPageFromFile(File &f, uint32_t startPos, bool draw, String *outText) {
  f.seek(startPos);

  u8g2.setFont(u8g2_font_7x14_tf);
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
  ensureBooksDir();

  File dir = FS.open("/books");
  if (!dir || !dir.isDirectory()) return;

  File f = dir.openNextFile();
  while (f && bookCount < MAX_BOOKS) {
    if (!f.isDirectory()) {
      String path = safePathFromDirEntry(String(f.name()));
      if (path.endsWith(".txt")) {
        books[bookCount].path = path;

        int slash = path.lastIndexOf('/');
        String fname = (slash >= 0) ? path.substring(slash + 1) : path;
        books[bookCount].name = stripTxtExt(fname);
        books[bookCount].name.replace('_', ' ');
        books[bookCount].size = f.size();

        bookCount++;
      }
    }
    f = dir.openNextFile();
  }

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

  currentBookKey = prefKeyForBook(path);

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
  const int y0 = H - STATUS_H;   // top of status bar

  size_t total = bookFile.size();
  if (total == 0) total = 1;

  int pageTextW = 0;
  if (SHOW_PAGE_NUMBER) {
    u8g2.setFont(PAGE_FONT);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", pageIndex + 1);
    pageTextW = u8g2.getUTF8Width(buf);

    int xTxt = W - MARGIN_X - pageTextW;
    int yTxt = H - 1;            // baseline ganz unten
    u8g2.setCursor(xTxt, yTxt);
    u8g2.print(buf);

    u8g2.setFont(u8g2_font_7x14_tf);
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
  u8g2.setFont(u8g2_font_7x14_tf);

  uint32_t nextOff = readPageFromFile(bookFile, start, true, nullptr);

  // Prefetch offsets ahead (speed)
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
//  Bookmarks storage (page-based)
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

void addBookmarkForCurrentBook() {
  if (currentBookKey.length() == 0) return;

  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(currentBookKey, pages);

  for (uint8_t i=0;i<count;i++){
    if ((int)pages[i] == pageIndex) {
      showModalBottomMessage("Bookmark exists", 1100);
      return;
    }
  }

  if (count < MAX_BOOKMARKS) pages[count++] = (uint16_t)pageIndex;
  else {
    for (uint8_t i=1;i<MAX_BOOKMARKS;i++) pages[i-1] = pages[i];
    pages[MAX_BOOKMARKS-1] = (uint16_t)pageIndex;
    count = MAX_BOOKMARKS;
  }

  // sort
  for (uint8_t i=0;i<count;i++){
    for (uint8_t j=i+1;j<count;j++){
      if (pages[j] < pages[i]) { uint16_t t=pages[i]; pages[i]=pages[j]; pages[j]=t; }
    }
  }

  saveBookmarksForKey(currentBookKey, pages, count);
  showModalBottomMessage("Bookmark saved", 1200);
}

// Local page offset helper (for bookmark snippets/web view)
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

  const uint8_t* FONT_NORMAL = u8g2_font_7x14_tf;
  const uint8_t* FONT_BOLD   = u8g2_font_7x14B_tf;

  u8g2.setFont(FONT_NORMAL);

  int ascent  = u8g2.getFontAscent();
  int descent = u8g2.getFontDescent();
  int lineH   = (ascent - descent) + LINE_GAP;

  int y = ascent + 4;

  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("Library");
#if HAS_BATTERY
  drawBatteryTopRight();
#endif
  y += lineH + 4;

  int totalItems = bookCount + 3;

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

    if (idx < bookCount) {
      u8g2.setFont(FONT_BOLD);
      u8g2.print(books[idx].name.c_str());
    } else {
      u8g2.setFont(FONT_NORMAL);
      if (idx == bookCount)        u8g2.print("Bookmarks");
      else if (idx == bookCount+1) u8g2.print("About");
      else                         u8g2.print("Upload");
    }

    y += lineH;
  }

  display.update();
}

void drawAbout() {
  prepareMenuFrame();
  u8g2.setFont(u8g2_font_7x14_tf);

  int ascent = u8g2.getFontAscent();
  int lineH = (ascent - u8g2.getFontDescent()) + LINE_GAP;
  int y = ascent + 4;

  u8g2.setCursor(MARGIN_X, y);
  u8g2.print("E-Book Reader");
#if HAS_BATTERY
  drawBatteryTopRight();
#endif
  y += lineH + 6;

  u8g2.setCursor(MARGIN_X, y); u8g2.print("1x: next / down"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("2x: open / back"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("Hold: bookmark");  y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("3x: Home");        y += lineH;

  display.update();
}

void drawBookmarksBookSelect() {
  prepareMenuFrame();

  const uint8_t* FONT_NORMAL = u8g2_font_7x14_tf;
  const uint8_t* FONT_BOLD   = u8g2_font_7x14B_tf;

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

  u8g2.setFont(u8g2_font_7x14_tf);
  int ascentTitle = u8g2.getFontAscent();
  int lineHTitle  = (ascentTitle - u8g2.getFontDescent()) + 2;

  int y = ascentTitle + 4;
  u8g2.setCursor(MARGIN_X, y);
  u8g2.print(books[bmBookIndex].name.c_str());

#if HAS_BATTERY
  drawBatteryTopRight();
#endif

  y += lineHTitle + 6;

  u8g2.setFont(u8g2_font_7x14_tf);
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

    String sn = readSnippetAtOffset(f, cursorOff);

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
String htmlPage() {
  return R"HTML(
<!doctype html><html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>EBOOK-READER</title>
</head>
<body style="font-family:system-ui;max-width:720px;margin:24px">
<h2>EBOOK-READER</h2>
<form method="POST" action="/upload" enctype="multipart/form-data">
<input type="file" name="file" accept=".txt,text/plain" required>
<button type="submit">Upload</button>
</form>
<p style="margin-top:18px">
  <a href="/files">Manage files</a> |
  <a href="/bookmarks">Bookmarks</a> |
  <a href="/reset">Factory reset</a>
</p>
</body></html>
)HTML";
}

void handleRoot(){ server.send(200, "text/html; charset=utf-8", htmlPage()); }

void handleFiles(){
  loadBooks();
  String out =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Files</title></head><body style='font-family:system-ui;max-width:720px;margin:24px'>"
    "<h2>Files</h2><p><a href='/'>Home</a> | <a href='/bookmarks'>Bookmarks</a></p><ul>";

  for(int i=0;i<bookCount;i++){
    out += "<li>";
    out += htmlEscape(books[i].name);
    out += " (";
    out += String((int)books[i].size);
    out += " bytes) ";
    out += "<a href='/del?id=" + String(i) + "' onclick=\"return confirm('Delete?')\">delete</a>";
    out += "</li>";
  }

  out += "</ul></body></html>";
  server.send(200, "text/html; charset=utf-8", out);
}

void handleDelete(){
  if (bookFile) bookFile.close();

  if(!server.hasArg("id")) { server.send(400,"text/plain","missing id"); return; }
  int id = server.arg("id").toInt();
  if(id < 0 || id >= bookCount) { server.send(400,"text/plain","bad id"); return; }

  String path = books[id].path;      // already "/books/..."
  if (FS.exists(path)) FS.remove(path);

  server.sendHeader("Location","/files");
  server.send(302,"text/plain","");
}

void handleUploadDone(){
  server.send(200, "text/plain", "Upload OK.\nShort press on device exits upload mode.");
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
  String out =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bookmarks</title>"
    "</head><body style='font-family:system-ui;max-width:820px;margin:24px'>"
    "<h2>Bookmarks</h2><p><a href='/'>Home</a> | <a href='/files'>Files</a></p>";

  for(int i=0;i<bookCount;i++){
    String key = prefKeyForBook(books[i].path);
    uint16_t pages[MAX_BOOKMARKS];
    uint8_t count = loadBookmarksForKey(key, pages);

    out += "<h3>";
    out += htmlEscape(books[i].name);
    out += "</h3>";

    if(count==0){
      out += "<p><em>No bookmarks</em></p>";
      continue;
    }

    File f = FS.open(books[i].path, "r");
    if (!f) { out += "<p><em>Open failed</em></p>"; continue; }

    out += "<ul>";
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

      String sn = readSnippetAtOffset(f, cursorOff);

      out += "<li>";
      out += "<strong>Bookmark " + String(j+1) + ":</strong> ";
      out += htmlEscape(sn);
      out += " &nbsp; ";
      out += "<a href='/viewbm?book=" + String(i) + "&idx=" + String(j) + "'>view</a>";
      out += " | ";
      out += "<a href='/delbm?book=" + String(i) + "&idx=" + String(j) +
             "' onclick=\"return confirm('Delete bookmark?')\">delete</a>";
      out += "</li>";
    }

    out += "</ul>";
    f.close();
  }

  out += "</body></html>";
  server.send(200, "text/html; charset=utf-8", out);
}

void handleDeleteBookmarkWeb(){
  if(!server.hasArg("book") || !server.hasArg("idx")) { server.send(400,"text/plain","missing book/idx"); return; }
  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if(b<0 || b>=bookCount) { server.send(400,"text/plain","bad book"); return; }

  String key = prefKeyForBook(books[b].path);
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if(idx<0 || idx>=count) { server.send(400,"text/plain","bad idx"); return; }

  for(int i=idx+1;i<count;i++) pages[i-1] = pages[i];
  count--;
  saveBookmarksForKey(key, pages, count);

  server.sendHeader("Location","/bookmarks");
  server.send(302,"text/plain","");
}

void handleViewBookmarkWeb() {
  if(!server.hasArg("book") || !server.hasArg("idx")) { server.send(400,"text/plain","missing book/idx"); return; }
  int b = server.arg("book").toInt();
  int idx = server.arg("idx").toInt();
  if(b<0 || b>=bookCount) { server.send(400,"text/plain","bad book"); return; }

  String key = prefKeyForBook(books[b].path);
  uint16_t pages[MAX_BOOKMARKS];
  uint8_t count = loadBookmarksForKey(key, pages);
  if(idx<0 || idx>=count) { server.send(400,"text/plain","bad idx"); return; }

  int page = (int)pages[idx];
  String txt = readPageTextForWeb(books[b].path, page);

  String out =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bookmark View</title></head>"
    "<body style='font-family:system-ui;max-width:920px;margin:24px'>";

  out += "<p><a href='/bookmarks'>← Back</a> | <a href='/files'>Files</a> | <a href='/'>Home</a></p>";
  out += "<h2>";
  out += htmlEscape(books[b].name);
  out += "</h2>";

  out += "<p><em>Bookmark ";
  out += String(idx+1);
  out += "</em></p>";

  out += "<pre style='white-space:pre-wrap;line-height:1.35;padding:12px;border:1px solid #ddd;border-radius:8px;'>";
  out += htmlEscape(txt);
  out += "</pre>";

  out += "</body></html>";
  server.send(200, "text/html; charset=utf-8", out);
}

void handleResetConfirm() {
  String out =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Factory Reset</title></head>"
    "<body style='font-family:system-ui;max-width:720px;margin:24px'>"
    "<h2>Factory Reset</h2>"
    "<p><strong>This will delete ALL books, bookmarks and reading progress.</strong></p>"
    "<form method='POST' action='/reset'>"
    "<button type='submit' style='padding:10px 14px'>Yes, reset</button>"
    "</form>"
    "<p style='margin-top:16px'><a href='/'>Back</a></p>"
    "</body></html>";

  server.send(200, "text/html; charset=utf-8", out);
}

void doFactoryReset() {
  // close any open book
  if (bookFile) bookFile.close();

  // clear preferences (bookmarks + progress)
  prefs.clear();

  // HARD WIPE LittleFS (this actually deletes all files)
  FS.end();
  delay(100);

  // format can take a moment
  FS.format();
  delay(200);

  // remount
  if (!FS.begin(true)) {
    // if this fails, at least don't crash
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
//  Upload mode start/stop
// ============================================================================
void startUploadMode(){
  mode = MODE_UPLOAD;

  prepareMenuFrame();
  u8g2.setFont(u8g2_font_7x14_tf);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  String url = String("http://") + ip.toString();

  int ascent=u8g2.getFontAscent(), descent=u8g2.getFontDescent();
  int lineH=(ascent - descent) + LINE_GAP;
  int y = TOP_PAD + ascent;

  u8g2.setCursor(MARGIN_X, y); u8g2.print("UPLOAD MODE"); y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(AP_SSID);      y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print(url.c_str());  y += lineH;
  u8g2.setCursor(MARGIN_X, y); u8g2.print("/files /bookmarks");

  display.update();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/del", HTTP_GET, handleDelete);

  server.on("/reset", HTTP_GET,  handleResetConfirm);
  server.on("/reset", HTTP_POST, handleResetDo);

  server.on("/bookmarks", HTTP_GET, handleBookmarksWeb);
  server.on("/delbm", HTTP_GET, handleDeleteBookmarkWeb);
  server.on("/viewbm", HTTP_GET, handleViewBookmarkWeb);

  server.on("/upload", HTTP_POST, handleUploadDone, [](){
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
  String fname = up.filename;

  // remove any path parts browsers might send
  int slash = fname.lastIndexOf('/');
  if (slash >= 0) fname = fname.substring(slash + 1);

  String clean;
  clean.reserve(fname.length());

  for (size_t i = 0; i < fname.length(); i++) {
    char c = fname[i];

    // allow letters, numbers, underscore, dash, space
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == ' ')
    {
      clean += c;
    }
    else if (c == '.') {
      clean += c;
    }
    else {
      clean += '_';
    }
  }
    // prevent weird paths
    clean.replace("..","");
    while (clean.startsWith(".")) clean.remove(0,1);

    if (!clean.endsWith(".txt")) clean += ".txt";

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

  // WiFi HARD OFF
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
  gfx.drawXBitmap(0, 0, pala_one_sleep_black_icon_v4_bits, W, H, 1);
  display.update();
}

// ============================================================================
//  Deep sleep
// ============================================================================
void goToSleep(){
  if (!ENABLE_DEEP_SLEEP) return;

  if (mode == MODE_READER && !bmPreviewActive) saveProgressThrottled(true);
  if (bookFile) bookFile.close();

  drawSleepScreen();
  delay(600);

  // HARD shut down radios
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();

  Platform::prepareToSleep();

  // IMPORTANT: disable ALL wake sources first
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // Wake only on button (GPIO0 LOW)
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

  // Lower CPU freq (80 MHz is plenty for e-ink + UI)
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
  loadBooks();

  lastUserActionMs = millis();
  drawLibrary();
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
      stopUploadModeToLibrary();
    } else {
      if (bookFile) bookFile.close();
      bmPreviewActive = false;
      mode = MODE_LIBRARY;
      drawLibrary();
    }
    return;
  }

  // UPLOAD
  if (mode == MODE_UPLOAD) {
    server.handleClient();
    if (btns.shortClick) stopUploadModeToLibrary();
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
    if (btns.veryLongClick) {
      mode = MODE_LIBRARY; drawLibrary(); return;
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
    if (btns.veryLongClick) { mode = MODE_LIBRARY; drawLibrary(); return; }
    return;
  }

  // BOOKMARK PREVIEW MODE
  if (mode == MODE_BM_PREVIEW) {
    if (btns.veryLongClick) {
      prefs.putInt((currentBookKey + "_p").c_str(), savedProgressPage);
      resetSaveThrottle();

      if (bookFile) bookFile.close();
      bmPreviewActive = false;
      mode = MODE_BM_LIST;
      drawBookmarksList();
      return;
    }

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
    int totalItems = bookCount + 3;
    // if (btns.veryLongClick) { startUploadMode(); return; }

    if (btns.shortClick) {
      selectedItem++;
      if (selectedItem >= totalItems) selectedItem = 0;
      drawLibrary();
      return;
    }

    if (btns.doubleClick) {
      if (selectedItem < bookCount) {
        if (openBookByIndex(selectedItem)) {
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

      if (selectedItem == bookCount) {
        bmBookIndex = 0;
        mode = MODE_BM_BOOK_SELECT;
        drawBookmarksBookSelect();
        return;
      }

      if (selectedItem == bookCount + 1) {
        mode = MODE_ABOUT;
        drawAbout();
        return;
      }

      startUploadMode();
      return;
    }
    return;
  }

  // READER (normal)
  if (mode == MODE_READER) {
    if (btns.longClick) {
      addBookmarkForCurrentBook();
      renderCurrentPage();
      return;
    }

    if (btns.shortClick) {
      pageIndex++;
      ensureOffsetsUpTo(pageIndex);
      if (eofReached && pageIndex >= knownPages) pageIndex = knownPages - 1;
      saveProgressThrottled(false);
      pageTurnsSinceFull++;
      renderCurrentPage();
      return;
    }

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

  idleLightSleepMaybe();
}