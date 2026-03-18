#include <Arduino.h>
#include <avr/pgmspace.h>

/*
  CUSTOM LCD: text + exact special signs
  ======================================

  Confirmed mapping:

  SCREEN LEFT:
    text columns 0..59
    special column 60:
      triangle1 = page0 bit4
      triangle2 = page0 bit7
      triangle3 = page1 bit1
      triangle4 = page1 bit3

  SCREEN RIGHT:
    text columns 0..73 (use 72 for 12 chars safely)
    special columns:
      col 74:
        A = page0 bit5
        B = page0 bit6
        C = page1 bit1
        D = page1 bit2

      col 75:
        R = page0 bit5
        S = page0 bit6
        X = page1 bit1
        Z = page1 bit2

      col 76:
        right triangle1 = page0 bit5
        right triangle2 = page0 bit6
        right triangle3 = page1 bit1
        right triangle4 = page1 bit2

  Physical routing already determined from your tests:
    SCREEN LEFT  -> physical CHIP_RIGHT
    SCREEN RIGHT -> physical CHIP_LEFT
*/

/* ============================================================
   Commands
   ============================================================ */
#define CMD_DISPLAY_OFF          0xAE
#define CMD_DISPLAY_ON           0xAF
#define CMD_DISPLAY_START_LINE   0xC0
#define CMD_SET_PAGE             0xB8
#define CMD_SET_COLUMN           0x00
#define CMD_ADC_FORWARD          0xA0
#define CMD_ADC_REVERSE          0xA1
#define CMD_STATIC_DRIVE_OFF     0xA4
#define CMD_DUTY_16              0xA8
#define CMD_RESET                0xE2

/* ============================================================
   Pins
   ============================================================ */
const uint8_t LCD_DB[8] = {2,3,4,5,6,7,8,9};

#define LCD_PIN_A0    10
#define LCD_PIN_MODE  11
#define LCD_PIN_STB   12
#define LCD_PIN_CS2   A1
#define LCD_PIN_RST   A2

/* ============================================================
   Geometry
   ============================================================ */
#define LCD_PAGES             2
#define LCD_RAM_COLS          80

#define FONT_CHAR_W           6

#define LEFT_TEXT_COLS        60
#define RIGHT_TEXT_COLS       72

#define LEFT_TEXT_CHARS       10
#define RIGHT_TEXT_CHARS      12

#define CHIP_LEFT             0
#define CHIP_RIGHT            1

#define LEFT_CHIP_IS_CS2      1

/* physical routing */
#define PHYS_CHIP_SCREEN_LEFT   CHIP_RIGHT
#define PHYS_CHIP_SCREEN_RIGHT  CHIP_LEFT

/* screen columns */
#define SCREEN_LEFT_TEXT_START_COL      0
#define SCREEN_LEFT_SPECIAL_COL         60

#define SCREEN_RIGHT_TEXT_START_COL     0
#define SCREEN_RIGHT_SPECIAL_COL_ABCD   74
#define SCREEN_RIGHT_SPECIAL_COL_RSXZ   75
#define SCREEN_RIGHT_SPECIAL_COL_TRI    76

/* ============================================================
   Settings
   ============================================================ */
#define RST_ACTIVE_HIGH       1
#define A0_DATA_HIGH          1
#define STB_ACTIVE_LOW        1
#define CS2_ACTIVE_LOW        1

#define LCD_ADC_REVERSED      0
#define FONT_LSB_TOP          1

/* ============================================================
   Timing
   ============================================================ */
#define T_SETUP_US            2
#define T_PULSE_US            2
#define T_HOLD_US             2
#define PAGE_PAUSE_US         200

/* ============================================================
   Buffers
   ============================================================ */
static uint8_t left_text_fb[LCD_PAGES][LEFT_TEXT_COLS];
static uint8_t right_text_fb[LCD_PAGES][RIGHT_TEXT_COLS];

static uint8_t left_spec_fb[LCD_PAGES];
static uint8_t right_abcd_fb[LCD_PAGES];
static uint8_t right_rsxz_fb[LCD_PAGES];
static uint8_t right_tri_fb[LCD_PAGES];

/* ============================================================
   Low level
   ============================================================ */
static inline void pinW(uint8_t pin, bool level)
{
  digitalWrite(pin, level ? HIGH : LOW);
}

static inline bool activeToLevel(bool active, bool activeLow)
{
  return activeLow ? !active : active;
}

static inline bool chipNeedsCS2Active(uint8_t chip)
{
#if LEFT_CHIP_IS_CS2
  return (chip == CHIP_LEFT);
#else
  return (chip == CHIP_RIGHT);
#endif
}

static inline void setA0(bool isData)
{
  pinW(LCD_PIN_A0, A0_DATA_HIGH ? isData : !isData);
}

static inline void setCS2(bool active)
{
  pinW(LCD_PIN_CS2, activeToLevel(active, CS2_ACTIVE_LOW));
}

static inline void setRST(bool active)
{
  pinW(LCD_PIN_RST, RST_ACTIVE_HIGH ? active : !active);
}

static inline void modeIdle(void)
{
  pinW(LCD_PIN_MODE, HIGH);
}

static inline void modeWriteWindowBegin(void)
{
  pinW(LCD_PIN_MODE, LOW);
}

static inline void modeWriteWindowEnd(void)
{
  pinW(LCD_PIN_MODE, HIGH);
}

static inline void stbIdle(void)
{
  pinW(LCD_PIN_STB, activeToLevel(false, STB_ACTIVE_LOW));
}

static inline void pulseStrobe(void)
{
  pinW(LCD_PIN_STB, activeToLevel(true, STB_ACTIVE_LOW));
  delayMicroseconds(T_PULSE_US);
  pinW(LCD_PIN_STB, activeToLevel(false, STB_ACTIVE_LOW));
}

static inline void setDataBus(uint8_t value)
{
  for (uint8_t i = 0; i < 8; i++)
    pinW(LCD_DB[i], (value >> i) & 1);
}

static void lcdWriteByte(uint8_t chip, bool isData, uint8_t value)
{
  setA0(isData);
  setDataBus(value);

  modeIdle();
  delayMicroseconds(1);

  modeWriteWindowBegin();
  setCS2(chipNeedsCS2Active(chip));

  delayMicroseconds(T_SETUP_US);
  pulseStrobe();

  setCS2(false);
  modeWriteWindowEnd();

  delayMicroseconds(T_HOLD_US);
}

static inline void lcdCmd(uint8_t chip, uint8_t cmd)
{
  lcdWriteByte(chip, false, cmd);
}

static inline void lcdData(uint8_t chip, uint8_t data)
{
  lcdWriteByte(chip, true, data);
}

/* ============================================================
   Base
   ============================================================ */
static void lcdResetHw(void)
{
  modeIdle();
  setCS2(false);
  setA0(false);
  stbIdle();

  setRST(true);
  delay(30);
  setRST(false);
  delay(60);
}

static void lcdSetPageColRaw(uint8_t chip, uint8_t page, uint8_t col)
{
  if (col >= LCD_RAM_COLS) col = LCD_RAM_COLS - 1;

  lcdCmd(chip, CMD_SET_PAGE | (page & 0x03));
  lcdCmd(chip, CMD_SET_COLUMN | (col & 0x7F));
}

static void lcdInitChip(uint8_t chip)
{
  lcdCmd(chip, CMD_DISPLAY_OFF);        delay(4);
  lcdCmd(chip, CMD_RESET);              delay(4);

  // SBN1661G-safe
  lcdCmd(chip, CMD_SET_PAGE | 0x00);    delay(4);
  lcdCmd(chip, CMD_SET_COLUMN | 0x00);  delay(4);

  lcdCmd(chip, CMD_DUTY_16);            delay(4);

#if LCD_ADC_REVERSED
  lcdCmd(chip, CMD_ADC_REVERSE);        delay(4);
#else
  lcdCmd(chip, CMD_ADC_FORWARD);        delay(4);
#endif

  lcdCmd(chip, CMD_STATIC_DRIVE_OFF);   delay(4);
  lcdCmd(chip, CMD_DISPLAY_START_LINE); delay(4);
  lcdCmd(chip, CMD_DISPLAY_ON);         delay(4);

  lcdCmd(chip, CMD_SET_PAGE | 0x00);    delay(4);
  lcdCmd(chip, CMD_SET_COLUMN | 0x00);  delay(4);
}

static void lcdInit(void)
{
  lcdResetHw();
  lcdInitChip(CHIP_LEFT);
  lcdInitChip(CHIP_RIGHT);
}

static void lcdClearChip(uint8_t chip)
{
  for (uint8_t page = 0; page < LCD_PAGES; page++)
  {
    lcdSetPageColRaw(chip, page, 0);
    for (uint8_t col = 0; col < LCD_RAM_COLS; col++)
      lcdData(chip, 0x00);

    delayMicroseconds(PAGE_PAUSE_US);
  }
}

static void lcdClearAll(void)
{
  lcdClearChip(CHIP_LEFT);
  lcdClearChip(CHIP_RIGHT);
}

/* ============================================================
   Buffer clear
   ============================================================ */
static void clearTextBuffers(void)
{
  for (uint8_t p = 0; p < LCD_PAGES; p++)
  {
    for (uint8_t i = 0; i < LEFT_TEXT_COLS; i++)
      left_text_fb[p][i] = 0x00;

    for (uint8_t i = 0; i < RIGHT_TEXT_COLS; i++)
      right_text_fb[p][i] = 0x00;
  }
}

static void clearSpecialBuffers(void)
{
  for (uint8_t p = 0; p < LCD_PAGES; p++)
  {
    left_spec_fb[p] = 0x00;
    right_abcd_fb[p] = 0x00;
    right_rsxz_fb[p] = 0x00;
    right_tri_fb[p] = 0x00;
  }
}

/* ============================================================
   Font
   ============================================================ */
static const uint8_t GLYPH_SPACE[5] PROGMEM = {0x00,0x00,0x00,0x00,0x00};
static const uint8_t GLYPH_DASH [5] PROGMEM = {0x08,0x08,0x08,0x08,0x08};
static const uint8_t GLYPH_DOT  [5] PROGMEM = {0x00,0x60,0x60,0x00,0x00};
static const uint8_t GLYPH_COLON[5] PROGMEM = {0x00,0x36,0x36,0x00,0x00};
static const uint8_t GLYPH_SLASH[5] PROGMEM = {0x20,0x10,0x08,0x04,0x02};

static const uint8_t GLYPH_0[5] PROGMEM = {0x3E,0x51,0x49,0x45,0x3E};
static const uint8_t GLYPH_1[5] PROGMEM = {0x00,0x42,0x7F,0x40,0x00};
static const uint8_t GLYPH_2[5] PROGMEM = {0x42,0x61,0x51,0x49,0x46};
static const uint8_t GLYPH_3[5] PROGMEM = {0x21,0x41,0x45,0x4B,0x31};
static const uint8_t GLYPH_4[5] PROGMEM = {0x18,0x14,0x12,0x7F,0x10};
static const uint8_t GLYPH_5[5] PROGMEM = {0x27,0x45,0x45,0x45,0x39};
static const uint8_t GLYPH_6[5] PROGMEM = {0x3C,0x4A,0x49,0x49,0x30};
static const uint8_t GLYPH_7[5] PROGMEM = {0x01,0x71,0x09,0x05,0x03};
static const uint8_t GLYPH_8[5] PROGMEM = {0x36,0x49,0x49,0x49,0x36};
static const uint8_t GLYPH_9[5] PROGMEM = {0x06,0x49,0x49,0x29,0x1E};

static const uint8_t GLYPH_A[5] PROGMEM = {0x7E,0x11,0x11,0x11,0x7E};
static const uint8_t GLYPH_B[5] PROGMEM = {0x7F,0x49,0x49,0x49,0x36};
static const uint8_t GLYPH_C[5] PROGMEM = {0x3E,0x41,0x41,0x41,0x22};
static const uint8_t GLYPH_D[5] PROGMEM = {0x7F,0x41,0x41,0x22,0x1C};
static const uint8_t GLYPH_E[5] PROGMEM = {0x7F,0x49,0x49,0x49,0x41};
static const uint8_t GLYPH_F[5] PROGMEM = {0x7F,0x09,0x09,0x09,0x01};
static const uint8_t GLYPH_G[5] PROGMEM = {0x3E,0x41,0x49,0x49,0x7A};
static const uint8_t GLYPH_H[5] PROGMEM = {0x7F,0x08,0x08,0x08,0x7F};
static const uint8_t GLYPH_I[5] PROGMEM = {0x00,0x41,0x7F,0x41,0x00};
static const uint8_t GLYPH_J[5] PROGMEM = {0x20,0x40,0x41,0x3F,0x01};
static const uint8_t GLYPH_K[5] PROGMEM = {0x7F,0x08,0x14,0x22,0x41};
static const uint8_t GLYPH_L[5] PROGMEM = {0x7F,0x40,0x40,0x40,0x40};
static const uint8_t GLYPH_M[5] PROGMEM = {0x7F,0x02,0x0C,0x02,0x7F};
static const uint8_t GLYPH_N[5] PROGMEM = {0x7F,0x04,0x08,0x10,0x7F};
static const uint8_t GLYPH_O[5] PROGMEM = {0x3E,0x41,0x41,0x41,0x3E};
static const uint8_t GLYPH_P[5] PROGMEM = {0x7F,0x09,0x09,0x09,0x06};
static const uint8_t GLYPH_Q[5] PROGMEM = {0x3E,0x41,0x51,0x21,0x5E};
static const uint8_t GLYPH_R[5] PROGMEM = {0x7F,0x09,0x19,0x29,0x46};
static const uint8_t GLYPH_S[5] PROGMEM = {0x46,0x49,0x49,0x49,0x31};
static const uint8_t GLYPH_T[5] PROGMEM = {0x01,0x01,0x7F,0x01,0x01};
static const uint8_t GLYPH_U[5] PROGMEM = {0x3F,0x40,0x40,0x40,0x3F};
static const uint8_t GLYPH_V[5] PROGMEM = {0x1F,0x20,0x40,0x20,0x1F};
static const uint8_t GLYPH_W[5] PROGMEM = {0x7F,0x20,0x18,0x20,0x7F};
static const uint8_t GLYPH_X[5] PROGMEM = {0x63,0x14,0x08,0x14,0x63};
static const uint8_t GLYPH_Y[5] PROGMEM = {0x03,0x04,0x78,0x04,0x03};
static const uint8_t GLYPH_Z[5] PROGMEM = {0x61,0x51,0x49,0x45,0x43};

static const uint8_t* lcdGlyph(char c)
{
  if (c >= 'a' && c <= 'z') c -= 32;

  switch (c)
  {
    case ' ': return GLYPH_SPACE;
    case '-': return GLYPH_DASH;
    case '.': return GLYPH_DOT;
    case ':': return GLYPH_COLON;
    case '/': return GLYPH_SLASH;

    case '0': return GLYPH_0;
    case '1': return GLYPH_1;
    case '2': return GLYPH_2;
    case '3': return GLYPH_3;
    case '4': return GLYPH_4;
    case '5': return GLYPH_5;
    case '6': return GLYPH_6;
    case '7': return GLYPH_7;
    case '8': return GLYPH_8;
    case '9': return GLYPH_9;

    case 'A': return GLYPH_A;
    case 'B': return GLYPH_B;
    case 'C': return GLYPH_C;
    case 'D': return GLYPH_D;
    case 'E': return GLYPH_E;
    case 'F': return GLYPH_F;
    case 'G': return GLYPH_G;
    case 'H': return GLYPH_H;
    case 'I': return GLYPH_I;
    case 'J': return GLYPH_J;
    case 'K': return GLYPH_K;
    case 'L': return GLYPH_L;
    case 'M': return GLYPH_M;
    case 'N': return GLYPH_N;
    case 'O': return GLYPH_O;
    case 'P': return GLYPH_P;
    case 'Q': return GLYPH_Q;
    case 'R': return GLYPH_R;
    case 'S': return GLYPH_S;
    case 'T': return GLYPH_T;
    case 'U': return GLYPH_U;
    case 'V': return GLYPH_V;
    case 'W': return GLYPH_W;
    case 'X': return GLYPH_X;
    case 'Y': return GLYPH_Y;
    case 'Z': return GLYPH_Z;
  }

  return GLYPH_SPACE;
}

static uint8_t lcdOrientGlyphByte(uint8_t b)
{
#if FONT_LSB_TOP
  return b;
#else
  uint8_t r = 0;
  for (uint8_t i = 0; i < 7; i++)
    if (b & (1 << i))
      r |= (1 << (6 - i));
  return r;
#endif
}

/* ============================================================
   Render text into raw column buffer
   ============================================================ */
static void renderTextToBuffer(uint8_t *dst, uint8_t cols, const char* s, uint8_t maxChars)
{
  for (uint8_t i = 0; i < cols; i++)
    dst[i] = 0x00;

  uint8_t x = 0;
  uint8_t chars = 0;

  while (*s && chars < maxChars && x < cols)
  {
    const uint8_t* g = lcdGlyph(*s);

    for (uint8_t i = 0; i < 6; i++)
    {
      uint8_t b = 0x00;
      if (i < 5) b = pgm_read_byte(g + i);
      b = lcdOrientGlyphByte(b);

      if (x >= cols) break;
      dst[x++] = b;
    }

    s++;
    chars++;
  }
}

/* ============================================================
   Public text API
   ============================================================ */
static void lcdShowTextSplit(
  const char* left1, const char* right1,
  const char* left2, const char* right2)
{
  clearTextBuffers();

  renderTextToBuffer(left_text_fb[0], LEFT_TEXT_COLS, left1, LEFT_TEXT_CHARS);
  renderTextToBuffer(right_text_fb[0], RIGHT_TEXT_COLS, right1, RIGHT_TEXT_CHARS);

  renderTextToBuffer(left_text_fb[1], LEFT_TEXT_COLS, left2, LEFT_TEXT_CHARS);
  renderTextToBuffer(right_text_fb[1], RIGHT_TEXT_COLS, right2, RIGHT_TEXT_CHARS);
}

/* ============================================================
   Exact named special-sign API
   ============================================================ */
static void lcdSetLeftTriangle(uint8_t idx, bool on)
{
  switch (idx)
  {
    case 1:
      if (on) left_spec_fb[0] |=  (1 << 4);
      else    left_spec_fb[0] &= ~(1 << 4);
      break;
    case 2:
      if (on) left_spec_fb[0] |=  (1 << 7);
      else    left_spec_fb[0] &= ~(1 << 7);
      break;
    case 3:
      if (on) left_spec_fb[1] |=  (1 << 0);
      else    left_spec_fb[1] &= ~(1 << 0);
      break;
    case 4:
      if (on) left_spec_fb[1] |=  (1 << 3);
      else    left_spec_fb[1] &= ~(1 << 3);
      break;
  }
}

static void lcdSetA(bool on)
{
  if (on) right_abcd_fb[0] |=  (1 << 5);
  else    right_abcd_fb[0] &= ~(1 << 5);
}

static void lcdSetB(bool on)
{
  if (on) right_abcd_fb[0] |=  (1 << 6);
  else    right_abcd_fb[0] &= ~(1 << 6);
}

static void lcdSetC(bool on)
{
  if (on) right_abcd_fb[1] |=  (1 << 1);
  else    right_abcd_fb[1] &= ~(1 << 1);
}

static void lcdSetD(bool on)
{
  if (on) right_abcd_fb[1] |=  (1 << 2);
  else    right_abcd_fb[1] &= ~(1 << 2);
}

static void lcdSetR(bool on)
{
  if (on) right_rsxz_fb[0] |=  (1 << 5);
  else    right_rsxz_fb[0] &= ~(1 << 5);
}

static void lcdSetS(bool on)
{
  if (on) right_rsxz_fb[0] |=  (1 << 6);
  else    right_rsxz_fb[0] &= ~(1 << 6);
}

static void lcdSetX(bool on)
{
  if (on) right_rsxz_fb[1] |=  (1 << 1);
  else    right_rsxz_fb[1] &= ~(1 << 1);
}

static void lcdSetZ(bool on)
{
  if (on) right_rsxz_fb[1] |=  (1 << 2);
  else    right_rsxz_fb[1] &= ~(1 << 2);
}

static void lcdSetRightTriangle(uint8_t idx, bool on)
{
  switch (idx)
  {
    case 1:
      if (on) right_tri_fb[0] |=  (1 << 5);
      else    right_tri_fb[0] &= ~(1 << 5);
      break;
    case 2:
      if (on) right_tri_fb[0] |=  (1 << 6);
      else    right_tri_fb[0] &= ~(1 << 6);
      break;
    case 3:
      if (on) right_tri_fb[1] |=  (1 << 1);
      else    right_tri_fb[1] &= ~(1 << 1);
      break;
    case 4:
      if (on) right_tri_fb[1] |=  (1 << 2);
      else    right_tri_fb[1] &= ~(1 << 2);
      break;
  }
}

/* ============================================================
   Composite update
   ============================================================ */
static void lcdUpdateAll(void)
{
  for (uint8_t page = 0; page < LCD_PAGES; page++)
  {
    // SCREEN RIGHT -> physical CHIP_LEFT
    lcdSetPageColRaw(PHYS_CHIP_SCREEN_RIGHT, page, SCREEN_RIGHT_TEXT_START_COL);
    for (uint8_t x = 0; x < RIGHT_TEXT_COLS; x++)
      lcdData(PHYS_CHIP_SCREEN_RIGHT, right_text_fb[page][x]);

    lcdSetPageColRaw(PHYS_CHIP_SCREEN_RIGHT, page, SCREEN_RIGHT_SPECIAL_COL_ABCD);
    lcdData(PHYS_CHIP_SCREEN_RIGHT, right_abcd_fb[page]);

    lcdSetPageColRaw(PHYS_CHIP_SCREEN_RIGHT, page, SCREEN_RIGHT_SPECIAL_COL_RSXZ);
    lcdData(PHYS_CHIP_SCREEN_RIGHT, right_rsxz_fb[page]);

    lcdSetPageColRaw(PHYS_CHIP_SCREEN_RIGHT, page, SCREEN_RIGHT_SPECIAL_COL_TRI);
    lcdData(PHYS_CHIP_SCREEN_RIGHT, right_tri_fb[page]);

    delayMicroseconds(PAGE_PAUSE_US);

    // SCREEN LEFT -> physical CHIP_RIGHT
    lcdSetPageColRaw(PHYS_CHIP_SCREEN_LEFT, page, SCREEN_LEFT_TEXT_START_COL);
    for (uint8_t x = 0; x < LEFT_TEXT_COLS; x++)
      lcdData(PHYS_CHIP_SCREEN_LEFT, left_text_fb[page][x]);

    lcdSetPageColRaw(PHYS_CHIP_SCREEN_LEFT, page, SCREEN_LEFT_SPECIAL_COL);
    lcdData(PHYS_CHIP_SCREEN_LEFT, left_spec_fb[page]);

    delayMicroseconds(PAGE_PAUSE_US);
  }
}

void LCD ()
{
  delay(1000);
  lcdUpdateAll();
}

/* ============================================================
   Arduino
   ============================================================ */
void setup()
{
  Serial.begin(115200);

  pinMode(LCD_PIN_A0, OUTPUT);
  pinMode(LCD_PIN_MODE, OUTPUT);
  pinMode(LCD_PIN_STB, OUTPUT);
  pinMode(LCD_PIN_CS2, OUTPUT);
  pinMode(LCD_PIN_RST, OUTPUT);

  for (uint8_t i = 0; i < 8; i++)
  {
    pinMode(LCD_DB[i], OUTPUT);
    digitalWrite(LCD_DB[i], LOW);
  }

  modeIdle();
  setCS2(false);
  setA0(false);
  stbIdle();
  setRST(false);

  delay(50);

  lcdInit();
  lcdClearAll();

  clearTextBuffers();
  clearSpecialBuffers();

  lcdShowTextSplit(
    "1234567890",
    "ABCDEFGHIJKL",
    "MNOPQRSTUV",
    "WXYZ01234567"
  );

  LCD();

  // demo
  lcdSetLeftTriangle(1, true);
  LCD();
  lcdSetLeftTriangle(2, true);
  LCD();
  lcdSetLeftTriangle(3, true);
  LCD();
  lcdSetLeftTriangle(4, true);
  LCD();

  lcdSetA(true);
  LCD();
  lcdSetB(true);
  LCD();
  lcdSetC(true);
  LCD();
  lcdSetD(true);
  LCD();

  lcdSetR(true);
  LCD();
  lcdSetS(true);
  LCD();
  lcdSetX(true);
  LCD();
  lcdSetZ(true);
  LCD();

  lcdSetRightTriangle(1, true);
  LCD();
  lcdSetRightTriangle(2, true);
  LCD();
  lcdSetRightTriangle(3, true);
  LCD();
  lcdSetRightTriangle(4, true);
  LCD();

  lcdUpdateAll();
}

void loop()
{
}