#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Zegar (czas lokalny). Płytka nie ma zegara z baterią - czas ginie po odłączeniu zasilania.
// Źródła: przeglądarka przy otwarciu strony WWW, ręczne ustawienie na ekranie, opcjonalny moduł
// DS3231 (I2C 0x68 na magistrali dotyku, SDA 19 / SCL 45) - jeśli jest, czas przetrwa wyłączenie.
void clock_init();            // po inicjalizacji dotyku (wspólna magistrala I2C)
bool clock_valid();           // czas ustawiony (rok >= 2024)
bool clock_has_rtc();         // wykryto DS3231
void clock_set(time_t local); // ustawia czas (i DS3231, jeśli jest)
bool clock_now(struct tm &out);
void clock_format(char *buf, size_t len); // "2026-11-01 15:32:45" albo "czas nieustawiony"
