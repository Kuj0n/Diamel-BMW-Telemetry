#pragma once

#define FW_VERSION "0.12"
#define FW_AUTHOR  "Diamel"
#define FW_VEHICLE "BMW F31 B47 xDrive (DDE D70BX7A0)"

// --- Ekran GUITION ESP32-4848S040 (ST7701, 480x480) ---
#define SCREEN_W 480
#define SCREEN_H 480
#define TFT_BL   38 // Podświetlenie (PWM)

// --- Dotyk GT911 (I2C); linie INT i RST nie są podłączone do ESP32 ---
#define TOUCH_SDA 19
#define TOUCH_SCL 45
// Korekta osi dotyku, jeśli punkt dotyku nie zgadza się z obrazem
#define TOUCH_SWAP_XY  0
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 0
// 1 = wypisuj współrzędne dotyku w Serial Monitorze
#define TOUCH_DEBUG 0

// --- Transceiver CAN (SN65HVD230) na złączu H1 ---
// TX -> H1 pin 3 (IO40), RX -> H1 pin 5 (IO2), 3.3V -> pad S3, GND -> H1 pin 2
#define CAN_TX_PIN 40
#define CAN_RX_PIN 2

// --- Karta microSD (SPI, piny 47/48 współdzielone z SPI inicjalizacji ekranu) ---
#define SD_CS   42
#define SD_MOSI 47
#define SD_SCK  48
#define SD_MISO 41
