/*
  ESP32 HUB75 Matrix Panel - Carwash Display
  64x32 P5 panellar uchun: 1/16 scan, 1/8 scan (four-scan) va ular aralash
  ustma-ust ulangan holat (bitta zanjirda 2 ta panel).

  Barcha chizish disp. orqali ketadi - u har bir panelning scan turiga qarab
  pikselni DMA buferidagi to'g'ri joyga o'tkazadi (pastdagi Screen klassi).

  Kerakli kutubxonalar:
    ESP32-HUB75-MatrixPanel-I2S-DMA  3.0.14
    GFX_Lite                         2.0.0
    ArduinoJson                      7.x
*/

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "font16x10.h"   // bitta panel (64x32)
#include "font_bahn.h"    // ikki panel (64x64)
#include <math.h>

#define PANEL_WIDTH    64
#define PANEL_HEIGHT   32
#define MAX_PANELS      2

// ---------- Panel sozlamasi ----------
// ESP32 panel turini o'zi aniqlay olmaydi: HUB75 bir tomonlama, paneldan hech
// qanday javob qaytmaydi. Shuning uchun tur Serial orqali BIR MARTA yuboriladi,
// flash xotirada saqlanadi va ESP32 qayta yuklanadi. Qayta proshivka shart emas.
//
//   {"panels":"16s"}                       bitta P5 2121-3264-16S-M5
//   {"panels":"8s"}                        bitta HYP5-1921-64x32-8S-H3.2
//   {"panels":"16s,8s"}                    ustma-ust: tepada 16S, pastda 8S
//   {"panels":"8s,16s","input":"bottom"}   ustma-ust, ESP32 kabeli pastki panelga ulangan
//   {"panels":"8s,8s","flip":"bottom"}     ustma-ust, pastki panel 180° aylantirilgan
//
// Ro'yxat har doim TEPADAN PASTGA yoziladi. "input" - ESP32 kabeli qaysi
// panelga ulangani ("top" yoki "bottom", standart "top"). "flip" - qaysi panel
// 180° aylantirib qo'yilgan ("none", "top", "bottom", "both", standart "none").
//
// Aralash zanjirda 16S panel ESP32 ga BIRINCHI ulanishi kerak: ESP32 -> 8S -> 16S
// ulanganda 16S umuman yonmadi (8S signallarni OUT ga uzatmaydi).
//
// Yorqinlik (darhol qo'llanadi va saqlanadi, qayta yuklash shart emas):
//   {"level":"100,60"}    har bir panel alohida, % yorug'lik, tepadan pastga (1..100)
//   {"brightness":120}    umumiy yorqinlik, 1..255
// Aralash zanjirda 8S qatori 16S bilan bir xil vaqt yonadi (pastga qarang), qolgan
// farqni (1921 LED yorug'roq) "level" bilan tenglashtiriladi.
// Panellar o'zgartirilganda "level" standart holatga qaytadi.
#define DEFAULT_PANELS      "16s"
#define DEFAULT_BRIGHTNESS  120      // P5 outdoor uchun 200 juda ko'p tok tortadi

// Pinout konfiguratsiyasi (v1.0.1)
#define RL1 18
#define GL1 17
#define BL1 16
#define RL2 15
#define GL2 19
#define BL2 21
#define CH_A 4
#define CH_B 22
#define CH_C 14
#define CH_D 13
#define CH_E 5
#define CLK 27
#define LAT 26
#define OE  25

MatrixPanel_I2S_DMA *dma_display = nullptr;
Preferences prefs;

// ---------- Ekran: panellar zanjiri ----------
// Kutubxona butun zanjirni bitta uzun gorizontal DMA qatori deb ko'radi:
//   16S panel -> qatorda 64 ustun, 16 manzil (A-B-C-D)
//   8S panel  -> qatorda 128 ustun, 8 manzil (A-B-C) - four-scan
// Zanjirda ESP32 ga ulangan panel DMA qatorining OXIRGI bo'lagini oladi
// (birinchi yuborilgan bit zanjirning eng uzoq uchiga yetib boradi).
struct PanelSlot {
  uint8_t scan;       // 16 yoki 8
  uint8_t level;      // shu panel yorqinligi, % yorug'lik (100 = to'liq)
  bool    flip;       // panel 180° aylantirib qo'yilgan
  int16_t dmaX0;      // DMA qatorida shu panelga tegishli birinchi ustun
  uint8_t lut[256];   // level uchun rang jadvali (setLevel to'ldiradi)
};

// Kutubxona har bir rangga CIE1931 korreksiyasini qo'llaydi (cie_luts.h):
//   L = v/255*100;  Y = L<=8 ? L/902.3 : ((L+16)/116)^3
// Rangni oddiy foizga ko'paytirish chiziqli emas: yorug' va xira ranglarda nisbat
// har xil chiqadi (MECANUZ nafas olganda farq o'zgarib turadi). Shuning uchun
// yorug'likni (Y) foizga ko'paytirib, qaytadan 8-bit qiymatga aylantiramiz.
static float cieY(float v) {
  float L = v * 100.0f / 255.0f;
  return (L <= 8.0f) ? L / 902.3f : powf((L + 16.0f) / 116.0f, 3.0f);
}

static uint8_t cieInv(float Y) {
  float L = (Y <= 8.0f / 902.3f) ? Y * 902.3f : 116.0f * cbrtf(Y) - 16.0f;
  float v = L * 255.0f / 100.0f + 0.5f;
  return v >= 255.0f ? 255 : (v <= 0.0f ? 0 : (uint8_t)v);
}

class Screen {
public:
  PanelSlot panel[MAX_PANELS];   // tepadan pastga
  uint8_t   count     = 0;
  int16_t   height    = PANEL_HEIGHT;   // umumiy ekran balandligi: 32 yoki 64
  int16_t   dmaWidth  = 0;
  int16_t   dmaHeight = 0;

  static int16_t scanWidth(uint8_t scan) { return scan == 8 ? PANEL_WIDTH * 2 : PANEL_WIDTH; }

  void setLevel(uint8_t i, uint8_t level) {
    panel[i].level = level;
    for (int v = 0; v < 256; v++) {
      panel[i].lut[v] = (level >= 100) ? v : cieInv(cieY(v) * level / 100.0f);
    }
  }

  void configure(const uint8_t *scan, uint8_t n, bool inputTop) {
    count     = n;
    height    = n * PANEL_HEIGHT;
    dmaWidth  = 0;
    dmaHeight = PANEL_HEIGHT / 2;       // faqat 8S bo'lsa: 8 manzil
    for (uint8_t i = 0; i < n; i++) {
      panel[i].scan = scan[i];
      dmaWidth += scanWidth(scan[i]);
      if (scan[i] == 16) dmaHeight = PANEL_HEIGHT;   // 16S bor: 16 manzil
    }

    int16_t x = dmaWidth;
    for (uint8_t k = 0; k < n; k++) {   // k - zanjirdagi o'rni (0 = ESP32 ga ulangan)
      uint8_t i = inputTop ? k : n - 1 - k;
      x -= scanWidth(scan[i]);
      panel[i].dmaX0 = x;
    }

    for (uint8_t i = 0; i < n; i++) {
      panel[i].flip = false;
      setLevel(i, 100);
    }
  }

  void drawPixel(int16_t x, int16_t y, uint16_t c) {
    if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= height) return;
    const PanelSlot &p = panel[y / PANEL_HEIGHT];
    y %= PANEL_HEIGHT;
    if (p.flip) {
      x = PANEL_WIDTH - 1 - x;
      y = PANEL_HEIGHT - 1 - y;
    }

    uint8_t r, g, b;
    MatrixPanel_I2S_DMA::color565to888(c, r, g, b);
    r = p.lut[r];
    g = p.lut[g];
    b = p.lut[b];

    if (p.scan == 16) {
      dma_display->drawPixelRGB888(p.dmaX0 + x, y, r, g, b);
      return;
    }

    // 8S: kutubxonadagi FOUR_SCAN_32PX_HIGH bilan bir xil xaritalash (128 x 16)
    int16_t dx = p.dmaX0 + x + ((y & 8) ? 0 : PANEL_WIDTH);
    int16_t dy = (y >> 4) * 8 + (y & 7);
    if (dmaHeight == PANEL_HEIGHT / 2) {
      dma_display->drawPixelRGB888(dx, dy, r, g, b);
      return;
    }

    // Zanjirda 16S ham bor -> DMA 16 manzil bo'yicha aylanadi, 8S esa D ni
    // ko'rmaydi: uning har bir qatori a va a+8 manzillarda ikki marta yonardi.
    // Faqat a manzilga yozamiz, a+8 da qora qoladi - 8S qatori 16S bilan bir xil
    // (1/16) vaqt yonadi, rang aniqligini yo'qotmasdan 2 barobar xiralashadi.
    // dy 0..7 -> R1 (DMA qatori = manzil), dy 8..15 -> R2 (DMA qatori = 16 + manzil)
    int16_t row = (dy < 8) ? dy : dy + 8;
    dma_display->drawPixelRGB888(dx, row, r, g, b);
  }

  // DMA buferidagi har bir nuqta qaysidir panelga tegishli, shuning uchun to'liq to'ldirish to'g'ri.
  // Faqat 0 (tozalash) bilan chaqiriladi - level bu yerda qo'llanmaydi.
  void fillScreen(uint16_t c) { dma_display->fillScreen(c); }
  void flipDMABuffer()        { dma_display->flipDMABuffer(); }
  static uint16_t color444(uint8_t r, uint8_t g, uint8_t b) { return MatrixPanel_I2S_DMA::color444(r, g, b); }
};

Screen disp;

char message[128]    = "MECANUZ";
char timeStr[6]      = "00:00";
char currencyStr[32] = "";

int   value          = 0;
int   xPos           = 0;

uint16_t textColor   = 0xFFFF;
uint16_t valueColor  = 0xFFFF;
String   displayType = "MECANUZ";
bool     isCyrillic  = false;

unsigned long valueZeroSince = 0;

#define FRAME_MS 50

// ---------- Shriftlar ----------
// Bitta panel (64x32): asl proporsional 16x10 shrift (font16x10.h) - p5_2121 bilan bir xil.
// Ikki panel (64x64): Bahnschrift Bold Condensed (font_bahn.h). Har bir qatorga
// sig'adigan eng katta o'lcham tanlanadi (24 -> 20 -> 15 px); 15 px da ham
// sig'masa - 20 px da skroll qiladi.
static const BahnFont *const FIT_FONTS[] = { &bahn24, &bahn20, &bahn15 };
#define SCROLL_FONT  bahn20
#define SPACE_WIDTH  5          // font16x10 uchun

uint16_t nextUTF8(const char* &ptr) {
  uint8_t b1 = *ptr;
  if (b1 == '\0') return 0;

  ptr++; // Advance by default

  if ((b1 & 0x80) == 0) {
    return b1;
  }

  if ((b1 & 0xE0) == 0xC0 || (b1 & 0xE0) == 0xD0) {
    uint8_t b2 = *ptr;
    if (b2 == '\0') return 0;
    ptr++;
    return ((b1 & 0x1F) << 6) | (b2 & 0x3F);
  }

  if ((b1 & 0xF0) == 0xE0) {
    uint8_t b2 = *ptr; if (b2 == '\0') return 0; ptr++;
    uint8_t b3 = *ptr; if (b3 == '\0') return 0; ptr++;
    return ((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
  }

  return b1;
}

// Lotin/kirill kichik harf -> katta; kirill rejimida lotin harflar kirillga o'giriladi
uint16_t canonicalCode(uint16_t code) {
  if (code >= 'a' && code <= 'z') code -= 32;

  if (isCyrillic) {
    if      (code == 'A') code = 0x0410; // А
    else if (code == 'B') code = 0x0411; // Б
    else if (code == 'V') code = 0x0412; // В
    else if (code == 'G') code = 0x0413; // Г
    else if (code == 'D') code = 0x0414; // Д
    else if (code == 'E') code = 0x0415; // Е
    else if (code == 'J') code = 0x0416; // Ж
    else if (code == 'Z') code = 0x0417; // З
    else if (code == 'I') code = 0x0418; // И
    else if (code == 'K') code = 0x041A; // К
    else if (code == 'L') code = 0x041B; // Л
    else if (code == 'M') code = 0x041C; // М
    else if (code == 'N') code = 0x041D; // Н
    else if (code == 'O') code = 0x041E; // О
    else if (code == 'P') code = 0x041F; // П
    else if (code == 'R') code = 0x0420; // Р
    else if (code == 'S') code = 0x0421; // С
    else if (code == 'T') code = 0x0422; // Т
    else if (code == 'U') code = 0x0423; // У
    else if (code == 'F') code = 0x0424; // Ф
    else if (code == 'X') code = 0x0425; // Х
    else if (code == 'H') code = 0x0425; // Х
    else if (code == 'C') code = 0x0426; // Ц
    else if (code == 'W') code = 0x0412; // В
    else if (code == 'Q') code = 0x041A; // К
    else if (code == 'Y') code = 0x042B; // Ы
  }

  if (code >= 0x0430 && code <= 0x044F) code -= 32;               // а..я -> А..Я
  if (code == 0x0451) code = 0x0401;                                // ё -> Ё
  if (code == 0x04A3 || code == 0x04AF || code == 0x04E9) code--;   // ң ү ө -> Ң Ү Ө
  return code;
}

int glyphIndex(const BahnFont &f, uint16_t code) {
  code = canonicalCode(code);
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t c = pgm_read_word(&f.codes[mid]);
    if (c == code) return mid;
    if (c < code) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

int spaceWidth(const BahnFont &f) { return f.cap / 3; }

int textWidth(const BahnFont &f, const char *str) {
  int width = 0;
  const char *ptr = str;
  while (*ptr) {
    uint16_t code = nextUTF8(ptr);
    if (code == 0) break;
    if (code == ' ') { width += spaceWidth(f); continue; }
    int i = glyphIndex(f, code);
    if (i >= 0) width += pgm_read_byte(&f.widths[i]) + 1;
  }
  return width > 0 ? width - 1 : 0;
}

// y - bosh harfning yuqori chizig'i
void drawText(const BahnFont &f, int16_t x, int16_t y, const char *str, uint16_t color) {
  const char *ptr = str;
  while (*ptr) {
    uint16_t code = nextUTF8(ptr);
    if (code == 0) break;
    if (code == ' ') { x += spaceWidth(f); continue; }
    int i = glyphIndex(f, code);
    if (i < 0) continue;

    uint8_t w = pgm_read_byte(&f.widths[i]);
    const uint32_t *rows = &f.bits[i * f.rows];
    for (int r = 0; r < f.rows; r++) {
      uint32_t line = pgm_read_dword(&rows[r]);
      if (!line) continue;
      for (int c = 0; c < w; c++) {
        if (line & (1UL << (w - 1 - c))) disp.drawPixel(x + c, y + r - f.capTop, color);
      }
    }
    x += w + 1;
  }
}

const BahnFont &fitFont(const char *str) {
  for (const BahnFont *f : FIT_FONTS) {
    if (textWidth(*f, str) <= PANEL_WIDTH) return *f;
  }
  return SCROLL_FONT;
}

// Ikki qatorli rejim: ikkala qator bir xil o'lchamda - ikkalasi sig'adigan eng kattasi.
// Hech birida sig'masa - eng kichigi (15 px), keng qator skroll qiladi.
const BahnFont &pairFont(const char *a, const char *b) {
  for (const BahnFont *f : FIT_FONTS) {
    if (textWidth(*f, a) <= PANEL_WIDTH && textWidth(*f, b) <= PANEL_WIDTH) return *f;
  }
  return *FIT_FONTS[sizeof(FIT_FONTS) / sizeof(FIT_FONTS[0]) - 1];
}


// ---------- font16x10 (bitta panel) ----------

int getFontIndex(uint16_t code) {
  code = canonicalCode(code);

  if (code >= '0' && code <= '9') return code - '0';
  if (code >= 'A' && code <= 'Z') return (code - 'A') + 10;
  if (code == ':') return 36;
  if (code == '-') return 37;
  if (code == '.') return 38;
  if (code == ',') return 39;
  if (code == '$') return 40;
  if (code == '%') return 41;
  if (code == '+') return 42;
  if (code == '/') return 43;
  if (code == '=') return 44;

  // Cyrillic Uppercase mapping (to match 81-character font16x10 array, skipping Ң, Ө, Ү)
  // Note: Cyrillic В uses its own glyph (index 47). The P4 version redirected it to
  // the Latin B glyph (index 11), but that one is only 13 rows tall while every
  // Cyrillic glyph is 14 - which made В sit one pixel short next to its neighbours.
  if (code >= 0x0410 && code <= 0x0415) return code - 0x0410 + 45; // А..Е
  if (code == 0x0401) return 51; // Ё
  if (code >= 0x0416 && code <= 0x041D) return code - 0x0416 + 52; // Ж..Н
  // Index 60 is Ң (skipped)
  if (code == 0x041E) return 61; // О
  // Index 62 is Ө (skipped)
  if (code >= 0x041F && code <= 0x0423) return code - 0x041F + 63; // П..У
  // Index 68 is Ү (skipped)
  if (code >= 0x0424 && code <= 0x042F) return code - 0x0424 + 69; // Ф..Я

  return -1; // Unknown character
}

// Belgining bo'sh ustunlarsiz chap/o'ng chegarasi
bool glyphBounds16(int index, int &left, int &right) {
  left = FONT16X10_WIDTH;
  right = -1;
  for (int row = 0; row < FONT16X10_HEIGHT; row++) {
    uint16_t line = pgm_read_word(&font16x10[index][row]);
    for (int col = 0; col < FONT16X10_WIDTH; col++) {
      if (line & (1 << (FONT16X10_WIDTH - 1 - col))) {
        if (col < left) left = col;
        if (col > right) right = col;
      }
    }
  }
  return right >= left;
}

int textWidth16(const char *str) {
  int width = 0;
  const char *ptr = str;
  while (*ptr) {
    uint16_t code = nextUTF8(ptr);
    if (code == 0) break;
    if (code == ' ') { width += SPACE_WIDTH; continue; }
    int index = getFontIndex(code), left, right;
    if (index >= 0 && glyphBounds16(index, left, right)) width += right - left + 2;
  }
  return width;
}

void drawText16(int16_t x, int16_t y, const char *str, uint16_t color) {
  const char *ptr = str;
  while (*ptr) {
    uint16_t code = nextUTF8(ptr);
    if (code == 0) break;
    if (code == ' ') { x += SPACE_WIDTH; continue; }
    int index = getFontIndex(code), left, right;
    if (index < 0 || !glyphBounds16(index, left, right)) continue;

    for (int row = 0; row < FONT16X10_HEIGHT; row++) {
      uint16_t line = pgm_read_word(&font16x10[index][row]);
      for (int col = left; col <= right; col++) {
        if (line & (1 << (FONT16X10_WIDTH - 1 - col))) disp.drawPixel(x + col - left, y + row, color);
      }
    }
    x += right - left + 2;
  }
}

// ---------- Layout ----------
// Eng tepa va eng past qator animatsiya chizig'i uchun band.
enum LineSlot { SLOT_SINGLE, SLOT_TOP, SLOT_BOTTOM };

// Matnni o'z joyiga chizadi va kengligini qaytaradi. 64 px dan keng matn
// (yoki scroll=true bo'lsa) xPos dan boshlab chiziladi - skrollni loop() qiladi.
// font berilmasa - matnga sig'adigan eng kattasi tanlanadi.
int drawLine(LineSlot slot, const char *text, uint16_t color, bool scroll = false,
             const BahnFont *font = nullptr) {
  if (disp.count == 1) {
    // 64x32: bitta qatorli matn 8-qatordan, ikki qatorli 1..14 va 17..30 qatorlarda
    int w = textWidth16(text);
    int16_t y = (slot == SLOT_SINGLE) ? 8 : (slot == SLOT_TOP) ? 1 : 17;
    int16_t x = (scroll || w > PANEL_WIDTH) ? xPos : (PANEL_WIDTH - w) / 2;
    drawText16(x, y, text, color);
    return w;
  }

  const BahnFont &f = font ? *font : fitFont(text);
  int w = textWidth(f, text);

  int16_t y;
  if (slot == SLOT_SINGLE) {
    y = 1 + (disp.height - 2 - f.cap) / 2;               // butun ekran o'rtasida
  } else {
    int16_t top = 1 + (PANEL_HEIGHT - 1 - f.cap) / 2;    // tepa panel o'rtasi
    y = (slot == SLOT_TOP) ? top : disp.height - top - f.cap;   // pastkisi - ko'zgu
  }

  int16_t x = (scroll || w > PANEL_WIDTH) ? xPos : (PANEL_WIDTH - w) / 2;
  drawText(f, x, y, text, color);
  return w;
}

// ---------- Yordamchi funksiyalar ----------

void formatTime(int rawValue, char* output) {
  int leftPart  = rawValue / 100;
  int rightPart = rawValue % 100;
  snprintf(output, 6, "%02d:%02d", leftPart, rightPart);
}

// 64px enda tort tomonlama ramka matnga joy qoldirmaydi (MECANUZ = 63px),
// shuning uchun animatsiya faqat tepa (0) va past (31) qatorlar boylab yuguradi.
#define ANIM_PERIMETER  (PANEL_WIDTH * 2)
#define ANIM_SNAKE_LEN  16
#define ANIM_STEP        2

void drawBorderAnimation() {
  if (displayType == "ERROR") {
    uint16_t errColor = ((millis() / 250) % 2 == 0) ? disp.color444(15, 0, 0) : 0;
    for (int i = 0; i < PANEL_WIDTH; i++) {
      disp.drawPixel(i, 0, errColor);
      disp.drawPixel(i, disp.height - 1, errColor);
    }
    return;
  }

  // Premium chiziq animatsiyasi (Snake)
  static int pos = 0;
  static uint8_t colorIndex = 0;
  uint16_t colors[] = {
    disp.color444(15, 0,  0),
    disp.color444(0,  15, 0),
    disp.color444(0,  0,  15),
    disp.color444(15, 15, 0),
    disp.color444(0,  15, 15),
    disp.color444(15, 0,  15)
  };

  uint16_t animColor1 = colors[colorIndex];
  uint16_t animColor2 = colors[(colorIndex + 2) % 6];

  // 0..63   -> tepa qator, chapdan ongga
  // 64..127 -> past qator, ongdan chapga (uzluksiz halqa hosil boladi)
  auto drawLinePixel = [&](int p, uint16_t c) {
    if (p < PANEL_WIDTH) {
      disp.drawPixel(p, 0, c);
    } else {
      disp.drawPixel(ANIM_PERIMETER - 1 - p, disp.height - 1, c);
    }
  };

  for (int i = 0; i < ANIM_SNAKE_LEN; i++) {
    drawLinePixel((pos + i) % ANIM_PERIMETER, animColor1);
    drawLinePixel((pos + i + (ANIM_PERIMETER / 2)) % ANIM_PERIMETER, animColor2);
  }

  pos += ANIM_STEP;
  if (pos >= ANIM_PERIMETER) {
    pos -= ANIM_PERIMETER;
    colorIndex = (colorIndex + 1) % 6;
  }
}

// Nafas oluvchi effekt
void animateMecanuz() {
  float breath = (sin(millis() / 400.0) + 1.0) / 2.0;
  int intensity = 2 + (13 * breath);
  textColor = disp.color444(intensity, intensity, intensity);
}

// Xato miltillashi
void animateError() {
  float breath = (sin(millis() / 200.0) + 1.0) / 2.0;
  int r = 5 + (10 * breath);
  textColor = disp.color444(r, 0, 0);
}

// ---------- Panel sozlamasi (flash xotirada) ----------

// "16s,8s" -> scan[] = {16, 8}. Xato bo'lsa 0 qaytaradi.
uint8_t parsePanels(const char *str, uint8_t *scan) {
  uint8_t n = 0;
  while (str && *str) {
    int s = atoi(str);
    if ((s != 16 && s != 8) || n >= MAX_PANELS) return 0;
    scan[n++] = s;
    str = strchr(str, ',');
    if (str) str++;
  }
  return n;
}

// "100,50" -> level[] (1..100 %). Soni panellar soniga teng bo'lmasa false.
bool parseLevels(const char *str, uint8_t *level) {
  uint8_t n = 0;
  while (str && *str) {
    int v = atoi(str);
    if (v < 1 || v > 100 || n >= disp.count) return false;
    level[n++] = v;
    str = strchr(str, ',');
    if (str) str++;
  }
  return n == disp.count;
}

uint8_t brightness = DEFAULT_BRIGHTNESS;

void printLevels() {
  Serial.print("Yorqinlik: umumiy ");
  Serial.print(brightness);
  Serial.print(", panellar (tepadan pastga) ");
  for (uint8_t i = 0; i < disp.count; i++) {
    Serial.printf("%s%d%%", i ? ", " : "", disp.panel[i].level);
  }
  Serial.println();
}

void loadPanelConfig() {
  prefs.begin("display", false);
  String panels = prefs.getString("panels", DEFAULT_PANELS);
  bool inputTop = prefs.getBool("inputTop", true);
  String levels = prefs.getString("lvl", "");      // eski "level" kaliti boshqa ma'noda edi
  String flip   = prefs.getString("flip", "none");
  brightness    = prefs.getUChar("bright", DEFAULT_BRIGHTNESS);
  prefs.end();

  uint8_t scan[MAX_PANELS];
  uint8_t n = parsePanels(panels.c_str(), scan);
  if (n == 0) {
    panels = DEFAULT_PANELS;
    n = parsePanels(DEFAULT_PANELS, scan);
  }
  disp.configure(scan, n, inputTop);

  uint8_t level[MAX_PANELS];
  if (parseLevels(levels.c_str(), level)) {
    for (uint8_t i = 0; i < n; i++) disp.setLevel(i, level[i]);
  }
  for (uint8_t i = 0; i < n; i++) {
    disp.panel[i].flip = flip == "both" || (flip == "top" && i == 0) ||
                         (flip == "bottom" && i == n - 1);
  }

  Serial.printf("Panellar (tepadan pastga): %s, ESP32 -> %s panel, flip: %s, DMA %dx%d\n",
                panels.c_str(), inputTop ? "tepa" : "pastki", flip.c_str(), disp.dmaWidth, disp.dmaHeight);
  printLevels();
}

// {"level":"100,50"} -> darhol qo'llanadi va flashga yoziladi
void saveLevels(const char *levels) {
  uint8_t level[MAX_PANELS];
  if (!parseLevels(levels, level)) {
    Serial.printf("Noto'g'ri level. %d ta panel uchun 1..100 %%, tepadan pastga. Misol: {\"level\":\"100,60\"}\n",
                  disp.count);
    return;
  }
  for (uint8_t i = 0; i < disp.count; i++) disp.setLevel(i, level[i]);

  prefs.begin("display", false);
  prefs.putString("lvl", levels);
  prefs.end();
  printLevels();
}

// {"brightness":150} -> darhol qo'llanadi va flashga yoziladi
void saveBrightness(int value) {
  brightness = constrain(value, 1, 255);
  dma_display->setBrightness8(brightness);

  prefs.begin("display", false);
  prefs.putUChar("bright", brightness);
  prefs.end();
  printLevels();
}

// {"panels":"16s,8s","input":"top","flip":"none"} -> flashga yoziladi va ESP32 qayta yuklanadi
void savePanelConfig(const char *panels, const char *input, const char *flip) {
  uint8_t scan[MAX_PANELS];
  if (parsePanels(panels, scan) == 0) {
    Serial.println("Noto'g'ri panels. Misol: {\"panels\":\"16s,8s\"}");
    return;
  }
  if (strcmp(flip, "none") && strcmp(flip, "top") && strcmp(flip, "bottom") && strcmp(flip, "both")) {
    Serial.println("Noto'g'ri flip. Qiymatlar: none, top, bottom, both");
    return;
  }

  prefs.begin("display", false);
  prefs.putString("panels", panels);
  prefs.putBool("inputTop", strcmp(input, "bottom") != 0);
  prefs.putString("flip", flip);
  prefs.remove("lvl");     // yangi panellar uchun standart yorqinlik
  prefs.end();

  Serial.println("Panel sozlamasi saqlandi, qayta yuklanmoqda...");
  Serial.flush();
  ESP.restart();
}

// ---------- JSON ----------

void parseJSON(String jsonStr) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  StaticJsonDocument<256> doc;
#endif
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.print("Buzilgan JSON: ");
    Serial.println(error.c_str());

    displayType = "ERROR";
    strcpy(message, "ERROR");
    strcpy(timeStr, "");
    return;
  }

  if (doc["panels"].is<const char*>()) {
    savePanelConfig(doc["panels"], doc["input"] | "top", doc["flip"] | "none");
    return;
  }
  if (doc["level"].is<const char*>()) {
    saveLevels(doc["level"]);
    return;
  }
  if (doc["brightness"].is<int>()) {
    saveBrightness(doc["brightness"]);
    return;
  }

  // Tilni aniqlash (kirill/lotin kalitlari orqali)
  const char* typeRaw = nullptr;

  if (doc["typekr"].is<const char*>()) {
    typeRaw = doc["typekr"];
    isCyrillic = true;
  } else if (doc["textkr"].is<const char*>()) {
    typeRaw = doc["textkr"];
    isCyrillic = true;
  } else if (doc["typekg"].is<const char*>()) {
    typeRaw = doc["typekg"];
    isCyrillic = true;
  } else if (doc["textkg"].is<const char*>()) {
    typeRaw = doc["textkg"];
    isCyrillic = true;
  } else if (doc["typeuz"].is<const char*>()) {
    typeRaw = doc["typeuz"];
    isCyrillic = false;
  } else if (doc["textuz"].is<const char*>()) {
    typeRaw = doc["textuz"];
    isCyrillic = false;
  } else {
    typeRaw = doc["type"] | "MECANUZ";
    isCyrillic = false;
  }

  int newValue = doc["value"] | 0;

  int r1 = doc["colorR1"] | 255;
  int g1 = doc["colorG1"] | 255;
  int b1 = doc["colorB1"] | 255;
  int r2 = doc["colorR2"] | 255;
  int g2 = doc["colorG2"] | 255;
  int b2 = doc["colorB2"] | 255;

  String typeStr = String(typeRaw);
  typeStr.toUpperCase();

  char newMsg[128] = "";
  String newDisplayType = "";

  if (typeStr == "CARWASH" || typeStr == "MECANUZ" || typeStr == "KGCARWASH" || typeStr == "KG") {
    newDisplayType = "MECANUZ";
    strcpy(newMsg, "MECANUZ");
  }
  else if (typeStr.startsWith("PP") && typeStr.endsWith("PP") && typeStr.length() > 4) {
    newDisplayType = "VALYUTA";
    String cleanType = typeStr.substring(2, typeStr.length() - 2);

    // Kirill rejimi uchun pul birligi avtomatik tarjimasi
    if (isCyrillic) {
      if (cleanType == "SUM" || cleanType == "SOM") {
        cleanType = "СОМ";
      }
    }

    strncpy(currencyStr, cleanType.c_str(), sizeof(currencyStr) - 1);
    snprintf(newMsg, sizeof(newMsg), "%d", newValue);
  }
  else if (typeStr.startsWith("KG") && typeStr.endsWith("KG") && typeStr.length() > 4) {
    newDisplayType = "VALYUTA";
    String cleanType = typeStr.substring(2, typeStr.length() - 2);
    strncpy(currencyStr, cleanType.c_str(), sizeof(currencyStr) - 1);
    snprintf(newMsg, sizeof(newMsg), "%d", newValue);
  }
  else if (typeStr == "SUM") {
    newDisplayType = "SUM";
    snprintf(newMsg, sizeof(newMsg), "%d", newValue);
  }
  else if (typeStr == "SOM") {
    newDisplayType = "VALYUTA";
    strcpy(currencyStr, isCyrillic ? "СОМ" : "SOM");
    snprintf(newMsg, sizeof(newMsg), "%d", newValue);
  }
  else if (typeStr == "СОМ" || typeStr == "сом") {
    newDisplayType = "VALYUTA";
    strcpy(currencyStr, "СОМ");
    snprintf(newMsg, sizeof(newMsg), "%d", newValue);
  }
  else if (typeStr == "TEST") {
    newDisplayType = "TEST";
    strcpy(newMsg, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ :-.,$%+/= АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ ҢӨҮ");
  }
  else {
    newDisplayType = "TEXT";

    // Kirill tili uchun sozlar tarjimasi
    if (isCyrillic && typeStr == "SHAMPUN") {
      strcpy(newMsg, "ШАМПУНЬ");
    }
    else if (isCyrillic && typeStr == "PAUZA") {
      strcpy(newMsg, "ПАУЗА");
    }
    else {
      strncpy(newMsg, typeStr.c_str(), sizeof(newMsg) - 1);
    }

    newMsg[sizeof(newMsg) - 1] = '\0';
    formatTime(newValue, timeStr);
  }

  textColor   = disp.color444(r1 / 17, g1 / 17, b1 / 17);
  valueColor  = disp.color444(r2 / 17, g2 / 17, b2 / 17);
  displayType = newDisplayType;
  value       = newValue;

  if (newDisplayType == "TEXT" && newValue == 0) {
    if (valueZeroSince == 0) valueZeroSince = millis();
  } else {
    valueZeroSince = 0;
  }

  // Matn ozgarganda xPos ni moslash
  if (strcmp(message, newMsg) != 0) {
    strcpy(message, newMsg);
    xPos = 0;   // sig'maydigan matn chap chetdan skroll qila boshlaydi

    // TEST rejimi uchun scroll ongdan boshlanadi
    if (displayType == "TEST") {
      xPos = PANEL_WIDTH;
    }
  }
}

// ---------- Setup ----------

void setup() {
  Serial.begin(115200);
  loadPanelConfig();

  HUB75_I2S_CFG::i2s_pins pins = { RL1, GL1, BL1, RL2, GL2, BL2, CH_A, CH_B, CH_C, CH_D, CH_E, LAT, OE, CLK };

  // Butun zanjir bitta uzun panel sifatida beriladi: 16S -> 64x32, 8S -> 128x16,
  // 16S + 8S -> 192x32
  HUB75_I2S_CFG cfg(disp.dmaWidth, disp.dmaHeight, 1, pins);
  cfg.i2sspeed    = HUB75_I2S_CFG::HZ_10M;   // 20M da bu panelda ghosting bolishi mumkin
  cfg.clkphase    = false;
  cfg.double_buff = true;

  dma_display = new MatrixPanel_I2S_DMA(cfg);
  if (!dma_display->begin()) {
    Serial.println("HUB75 DMA ishga tushmadi (xotira yetmadi?)");
  }
  dma_display->setBrightness8(brightness);
  dma_display->clearScreen();

  strcpy(message, "MECANUZ");
  displayType = "MECANUZ";
}

// ---------- Loop ----------

void loop() {
  static String inputBuffer = "";
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) parseJSON(inputBuffer);
      inputBuffer = "";
    } else {
      inputBuffer += c;
    }
  }

  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < FRAME_MS) return;
  lastFrame = millis();

  disp.fillScreen(0);
  drawBorderAnimation();

  if (displayType == "MECANUZ") {
    animateMecanuz();
    drawLine(SLOT_SINGLE, message, textColor);
  }
  else if (displayType == "ERROR") {
    animateError();
    drawLine(SLOT_SINGLE, message, textColor);
  }
  else if (displayType == "SUM") {
    drawLine(SLOT_SINGLE, message, valueColor);
  }
  else if (displayType == "VALYUTA") {
    const BahnFont &f = pairFont(message, currencyStr);
    drawLine(SLOT_TOP, message, valueColor, false, &f);
    drawLine(SLOT_BOTTOM, currencyStr, textColor, false, &f);
  }
  else if (displayType == "TEST") {
    // Ikki panelda alifbo ikkala panel o'rtasidan skroll qiladi
    int w = drawLine(SLOT_SINGLE, message, textColor, true);
    xPos--;
    if (xPos < -w) {
      xPos = PANEL_WIDTH;
    }
  }
  else {
    bool hideTime = (value == 0 && valueZeroSince != 0 && (millis() - valueZeroSince >= 1000));

    int w;
    if (hideTime) {
      w = drawLine(SLOT_SINGLE, message, textColor);
    } else if (strcmp(message, "ERROR") == 0) {
      w = drawLine(SLOT_TOP, message, textColor);
    } else {
      const BahnFont &f = pairFont(message, timeStr);
      w = drawLine(SLOT_TOP, message, textColor, false, &f);
      drawLine(SLOT_BOTTOM, timeStr, valueColor, false, &f);
    }

    // SKROLL MANTIQI: Uzun matnlar qotib qolmasligi uchun uzluksiz skroll boladi
    if (w > PANEL_WIDTH) {
      xPos--;
      // Matn toliq chiqib ketgach, yana ong tarafdan kirib kelishni boshlaydi
      if (xPos < -w) {
        xPos = PANEL_WIDTH;
      }
    }
  }

  disp.flipDMABuffer();
}
