#include "touch_gt911.h"
#include <Arduino.h>
#include <Wire.h>

#define GT911_REG_CONFIG  0x8047 // config_version, potem x_max (LE), y_max (LE)
#define GT911_REG_PRODUCT 0x8140 // ID produktu, np. "911"
#define GT911_REG_STATUS  0x814E // bit7 = nowe dane, bity 0-3 = liczba punktów
#define GT911_REG_POINT1  0x8150 // x (LE), y (LE) pierwszego punktu

static uint8_t addr = 0;
static uint16_t max_x = 0, max_y = 0;
#define RELEASE_TIMEOUT_MS 80 // GT911 raportuje co ~10 ms, dopóki palec jest na ekranie

static bool pressed = false;
static uint16_t last_x = 0, last_y = 0;
static uint32_t last_data_ms = 0;

static bool read_reg(uint16_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(addr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static void write_reg(uint16_t reg, uint8_t value) {
    Wire.beginTransmission(addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(value);
    Wire.endTransmission();
}

bool gt911_begin(int sda, int scl) {
    Wire.begin(sda, scl, 400000);

    // Adres GT911 zależy od stanu linii INT przy starcie: 0x5D albo 0x14
    const uint8_t candidates[] = {0x5D, 0x14};
    for (uint8_t a : candidates) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            addr = a;
            break;
        }
    }
    if (!addr) return false;

    uint8_t id[4] = {0};
    read_reg(GT911_REG_PRODUCT, id, 4);
    uint8_t cfg[5] = {0};
    if (read_reg(GT911_REG_CONFIG, cfg, 5)) {
        max_x = cfg[1] | (cfg[2] << 8);
        max_y = cfg[3] | (cfg[4] << 8);
    }
    Serial.printf("Dotyk: GT911 pod adresem 0x%02X, ID '%c%c%c%c', rozdzielczosc %ux%u\n",
                  addr, id[0], id[1], id[2], id[3] ? id[3] : ' ', max_x, max_y);
    return true;
}

bool gt911_read(uint16_t &x, uint16_t &y) {
    if (!addr) return false;

    uint8_t status;
    if (read_reg(GT911_REG_STATUS, &status, 1) && (status & 0x80)) {
        last_data_ms = millis();
        uint8_t points = status & 0x0F;
        uint8_t p[4];
        if (points > 0 && read_reg(GT911_REG_POINT1, p, 4)) {
            last_x = p[0] | (p[1] << 8);
            last_y = p[2] | (p[3] << 8);
            pressed = true;
        } else {
            pressed = false;
        }
        write_reg(GT911_REG_STATUS, 0); // potwierdzenie odczytu
    } else if (pressed && millis() - last_data_ms > RELEASE_TIMEOUT_MS) {
        // Kontroler przestał raportować, a raport "0 punktów" nie przyszedł - palec puszczony
        pressed = false;
    }
    x = last_x;
    y = last_y;
    return pressed;
}

uint8_t gt911_address() { return addr; }
uint16_t gt911_max_x() { return max_x; }
uint16_t gt911_max_y() { return max_y; }
