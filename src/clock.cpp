#include "clock.h"
#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include "settings.h"

#define DS3231_ADDR 0x68

static bool rtc = false;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static void set_system(time_t t) {
    struct timeval tv = {t, 0};
    settimeofday(&tv, nullptr); // czas lokalny trzymany jako "UTC" - bez stref czasowych
}

static bool rtc_read(struct tm &tm) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0 || Wire.requestFrom(DS3231_ADDR, 7) != 7) return false;
    uint8_t r[7];
    for (uint8_t &b : r) b = Wire.read();
    memset(&tm, 0, sizeof(tm));
    tm.tm_sec = bcd2bin(r[0] & 0x7F);
    tm.tm_min = bcd2bin(r[1] & 0x7F);
    tm.tm_hour = bcd2bin(r[2] & 0x3F);
    tm.tm_mday = bcd2bin(r[4] & 0x3F);
    tm.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
    tm.tm_year = bcd2bin(r[6]) + 100; // 2000 + rok
    return true;
}

static void rtc_write(const struct tm &tm) {
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x00);
    Wire.write(bin2bcd(tm.tm_sec));
    Wire.write(bin2bcd(tm.tm_min));
    Wire.write(bin2bcd(tm.tm_hour)); // tryb 24 h
    Wire.write(bin2bcd(tm.tm_wday + 1));
    Wire.write(bin2bcd(tm.tm_mday));
    Wire.write(bin2bcd(tm.tm_mon + 1));
    Wire.write(bin2bcd(tm.tm_year % 100));
    Wire.endTransmission();
}

void clock_init() {
    rtc = false;
    if (!g_settings.rtc_installed) {
        Serial.println("Zegar: brak modulu RTC (wg ustawien) - czas z przegladarki albo recznie");
        return;
    }
    Wire.beginTransmission(DS3231_ADDR);
    rtc = Wire.endTransmission() == 0;
    struct tm tm;
    if (rtc && rtc_read(tm) && tm.tm_year >= 124) {
        set_system(mktime(&tm));
        Serial.println("Zegar: czas z modulu DS3231");
    } else {
        Serial.printf("Zegar: %s - czas zostanie ustawiony z przegladarki albo recznie\n",
                      rtc ? "DS3231 bez ustawionego czasu" : "NIE WYKRYTO modulu DS3231 (0x68)");
    }
}

bool clock_valid() {
    struct tm tm;
    return clock_now(tm);
}

bool clock_has_rtc() { return rtc; }

void clock_set(time_t local) {
    set_system(local);
    struct tm tm;
    gmtime_r(&local, &tm);
    if (rtc) rtc_write(tm);
}

bool clock_now(struct tm &out) {
    time_t t = time(nullptr);
    gmtime_r(&t, &out);
    return out.tm_year >= 124; // >= 2024: czas ustawiony
}

void clock_format(char *buf, size_t len) {
    struct tm tm;
    if (clock_now(tm)) strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
    else snprintf(buf, len, "czas nieustawiony");
}
