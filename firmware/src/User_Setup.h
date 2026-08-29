// User_Setup.h — TFT_eSPI configuration for ESP32-2432S028R (CYD)
// Copy to src/ for PlatformIO, or reference via build_flags -I flag

// ──────────────────────────────────────────────
// Display Controller
// CYD2USB (dual USB-C + USB-B revision) uses ST7789, not ILI9341
// ──────────────────────────────────────────────
#define ST7789_DRIVER
#define USE_HSPI_PORT

// ──────────────────────────────────────────────
// Display Dimensions
// ──────────────────────────────────────────────
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ──────────────────────────────────────────────
// TFT Pinout (ESP32-2432S028R / CYD)
// ──────────────────────────────────────────────
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15   // Display chip select
#define TFT_DC    2   // Data/Command
#define TFT_RST  -1   // Not GPIO-wired on CYD2USB (tied to EN)
#define TFT_BL   21   // Backlight
#define TFT_BACKLIGHT_ON HIGH  // Required for TFT_eSPI to auto-drive TFT_BL

// ──────────────────────────────────────────────
// Display Options
// ──────────────────────────────────────────────
#define TFT_INVERSION_OFF
#define TFT_RGB_ORDER TFT_BGR

// ──────────────────────────────────────────────
// SPI Speed
// ──────────────────────────────────────────────
#define SPI_FREQUENCY   55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY 2500000

// ──────────────────────────────────────────────
// Touch Controller (XPT2046) — separate SPI bus, NOT TFT_eSPI's
// CLK=GPIO25, MOSI=GPIO32, MISO=GPIO39, CS=GPIO33, IRQ=GPIO36
// Handled independently in main.cpp via XPT2046_Touchscreen
// ──────────────────────────────────────────────

// ──────────────────────────────────────────────
// SD Card — own SPI bus (ESP32 default VSPI pins)
// CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCLK=GPIO18
// Handled separately in SD.begin()
// ──────────────────────────────────────────────

// ──────────────────────────────────────────────
// Font Support
// ──────────────────────────────────────────────
#define LOAD_GLCD
#define FONT_1
#define FONT_2
#define FONT_4
#define FONT_6
#define FONT_7
#define FONT_8

// ──────────────────────────────────────────────
// Debug
// ──────────────────────────────────────────────
// #define TFT_DEBUG
// #define SPI1DMA_EXAMPLE
