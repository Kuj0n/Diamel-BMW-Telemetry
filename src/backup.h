#pragma once
#include <stddef.h>
#include "settings.h"

// Kopia ustawień w pliku JSON. Parametry zapisane po nazwie ("12:motordrehzahl_wert"), więc kopia działa
// także po wgraniu nowego katalogu. Zawiera hasła Wi-Fi - plik trzymaj u siebie.

// Ustawienia -> tekst JSON (w PSRAM; zwolnić free())
char *backup_to_json(size_t *len);
// JSON -> kopia ustawień (na bazie bieżących); false + opis błędu. skipped = parametry, których nie ma w katalogu
bool backup_from_json(const char *json, size_t len, Settings &out, int &skipped, char *err, size_t err_len);

// Karta SD: plik SET_<data>.json w katalogu logów (widoczny w Plikach i do pobrania przez WWW)
bool backup_save_sd(char *name, size_t len);
// Najnowszy plik SET_*.json z karty -> ustawienia zapisane w pamięci (potem restart)
bool backup_restore_sd(char *name, size_t name_len, char *err, size_t err_len);
