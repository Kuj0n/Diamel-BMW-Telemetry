# Diamel BMW Telemetry

Wyświetlacz telemetrii do BMW serii F (testowany projekt: **F31 320d xDrive, silnik B47, sterownik DDE D70BX7A0**), zbudowany na 4-calowym dotykowym panelu z ESP32-S3. Podłącza się do gniazda OBD, czyta na żywo wartości ze sterowników auta i pokazuje je na ekranie, na wykresie oraz w przeglądarce telefonu.

**Tylko odczyt.** Urządzenie niczego w aucie nie zapisuje, nie koduje i nie kasuje błędów. Blokada jest w kodzie: na magistralę mogą trafić wyłącznie usługi odczytu (UDS 0x22, 0x19, 0x3E).

> Status: **wersja rozwojowa (0.12)**. Interfejs, katalog parametrów, Wi-Fi i strona WWW działają w trybie DEMO. Odczyt z auta przez CAN jest zaimplementowany, ale **nie był jeszcze testowany w samochodzie**.

<!-- Zrzuty ekranu: docs/screenshots/ (zrób je funkcją "Zrzut ekranu" na stronie WWW urządzenia) -->

## Możliwości

- **3 strony odczytów** (przesuwane palcem), na każdej do 12 wartości (2 × 6), każda z własnym zestawem parametrów
- **Wykres** do 3 serii z automatyczną skalą (druga oś dla innej jednostki)
- **Katalog ponad 10 000 parametrów** z 21 sterowników auta (silnik, skrzynia, xDrive, DSC, klimatyzacja, AdBlue, licznik…), w tym **lista podstawowa ~80 wartości z polskimi nazwami** (obroty, doładowanie, rail, temperatury, DPF, EGR, momenty xDrive, zużycie sprzęgła xDrive, AdBlue…)
- **Odczyt kodów błędów** na żądanie, z opisami, eksport do CSV
- **Log CSV na kartę microSD** (format Excel PL albo standardowy), nazwy plików z datą
- **Wi-Fi**: własna sieć wyświetlacza (kod QR na ekranie) albo połączenie z siecią domową (`http://diamel.local`)
- **Strona WWW**: podgląd na żywo, konfiguracja stron i wykresu, pliki (pobieranie/usuwanie), formatowanie karty, kopia ustawień (JSON), aktualizacja oprogramowania (OTA), **zrzuty ekranu i nagrywanie wideo z ekranu**
- **Automatyczne usypianie** wg dowolnego parametru (np. obroty < 1 przez 5 min albo brak odpowiedzi sterownika po wyłączeniu zapłonu); dotyk budzi
- **Tryb DEMO** z symulowanymi danymi - do testów przy biurku
- Zegar ustawiany z przeglądarki (opcjonalnie moduł RTC DS3231)

## Sprzęt

| Element | Opis |
|---|---|
| Wyświetlacz | **Guition / Sunton ESP32-4848S040**: 4.0" IPS 480×480 (ST7701, RGB), dotyk GT911, ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM), gniazdo microSD |
| Transceiver CAN | moduł **SN65HVD230** (3,3 V) - **zdejmij z modułu rezystor 120 Ω (R2)**: auto ma już terminację |
| Wtyczka | męska wtyczka OBD-II (potrzebne 3 styki) |
| Zasilanie | ładowarka USB-C do gniazda zapalniczki |
| Karta | microSD (FAT32) - logi i katalog parametrów |

### Połączenia

| Moduł SN65HVD230 | Płytka ESP32-4848S040 |
|---|---|
| 3V3 | pad **S3** (3,3 V) |
| GND | złącze **H1, pin 2** (GND) |
| CTX (TX) | złącze **H1, pin 3** (IO40) |
| CRX (RX) | złącze **H1, pin 5** (IO2) |

| Moduł SN65HVD230 | Gniazdo OBD-II w aucie |
|---|---|
| CANH | **pin 6** |
| CANL | **pin 14** |
| GND | **pin 4 lub 5** (masa) - koniecznie, nawet przy zasilaniu z USB |

CANH i CANL poprowadź skrętką. Pin 16 (+12 V) nie jest potrzebny.

**Test bez auta:** Menu → Info → *Test CAN* sprawdza tor TX → transceiver → RX (wysyła ramki - tylko z wtyczką **odłączoną** od auta). Na biurku, bez terminacji, może być potrzebny tymczasowy rezystor 120 Ω między CANH i CANL.

### Opcjonalnie: zegar RTC (DS3231)

Płytka nie ma zegara z baterią - czas ustawia się z przeglądarki przy otwarciu strony WWW. Moduł DS3231 podłącza się do magistrali I2C dotyku (wg schematu płytki):

| DS3231 | Płytka |
|---|---|
| VCC | 3,3 V (pad S3 albo złącze P2, pin 6) |
| GND | H1 pin 2 albo P2 pin 5 |
| SDA | rezystor **R4** (4,7 kΩ) - strona **niepołączona** z 3,3 V (IO19) |
| SCL | rezystor **R3** (4,7 kΩ) - strona **niepołączona** z 3,3 V (IO45) |

Przed lutowaniem sprawdź multimetrem, która strona rezystora to 3,3 V. Potem w Ustawieniach zaznacz *Moduł zegara RTC zainstalowany*.

## Oprogramowanie

Projekt [PlatformIO](https://platformio.org/) (Arduino, ESP32-S3). Wersje platformy i bibliotek są przypięte w `platformio.ini`.

- LVGL 8.4 + Arduino_GFX (ekran), własny sterownik GT911 (dotyk)
- TWAI (CAN) + ISO-TP z adresowaniem rozszerzonym BMW (tester 0x6F1, odpowiedzi 0x600 + adres sterownika)
- ESPAsyncWebServer + ArduinoJson (strona WWW), NimBLE (adapter BLE)

### Kompilacja i wgranie

1. Zainstaluj VS Code + rozszerzenie PlatformIO.
2. Otwórz folder projektu, podłącz płytkę kablem USB-C (port z konwerterem CH340).
3. *PlatformIO: Upload* (pierwsza kompilacja trwa kilka minut - pobiera biblioteki).

Kolejne wersje możesz wgrywać bez kabla: strona WWW → *System* → *Aktualizacja oprogramowania* (plik `.pio/build/esp32-s3/firmware.bin` albo plik z [Releases](../../releases)).

### Pierwsze uruchomienie

1. Na ekranie: Menu → **Wi-Fi** → *Włącz*. Zeskanuj kod QR telefonem i otwórz adres z ekranu (`http://192.168.4.1`).
2. W zakładce **System** wgraj katalog parametrów (patrz niżej) i uruchom urządzenie ponownie (Info → *Uruchom ponownie*).
3. W **Ustawieniach** wybierz tryb pracy **CAN - gniazdo OBD**, a w menu naciśnij **Start**.

## Katalog parametrów

Opisy parametrów i błędów pochodzą z plików opisu sterowników BMW (`.prg`, format EDIABAS). **Te pliki są własnością BMW i nie są częścią projektu** - katalog generujesz ze swoich plików:

```
python tools/wyciagnij_f31.py <folder_z_plikami_prg> <folder_wyjsciowy>
```

Skrypt czyta tylko sterowniki z listy `CAR_ECUS` (dla innego auta zmień ją - nazwy plików SGBD pokaże np. identyfikacja pojazdu w BimmerDeepOBD), bierze adresy z `T_GRTB.PRG`, wyciąga parametry odczytu (usługa 0x22) z tabel oraz - przez `tools/best_dekoder.py` - z kodu zadań diagnostycznych (np. skrzynia rozdzielcza xDrive), a nazwy tłumaczy na polski. Wynik (`moduly.csv`, `parametry.csv`, `teksty.csv`, `bledy_XX.csv`) wgrywasz przez stronę WWW: *System* → *Katalog parametrów* - trafia na kartę do `/diamel`.

## Do zrobienia

- [ ] **Test w aucie**: weryfikacja odczytów CAN, czasów odpowiedzi i przeliczeń wartości
- [ ] **Odczyt przez adapter BLE (ELM327)** - wyszukiwanie, połączenie i test adaptera już są; brakuje zapytań UDS przez adapter (wymaga adaptera z Bluetooth LE, np. vLinker MC+, OBDLink CX)
- [ ] **Zegar RTC DS3231** - obsługa jest, brak sprawdzenia na sprzęcie
- [ ] Procent spięcia xDrive (wyliczany z momentu / kąta zapięcia sprzęgła)
- [ ] Wskaźnik regeneracji DPF, wartości min/max, progi ostrzegawcze
- [ ] Strona ze wskaźnikami (zegary analogowe)
- [ ] Lepsze tłumaczenia pełnego katalogu (poza listą podstawową)
- [ ] Profile innych aut (np. OBD-II dla aut spoza BMW)
- [ ] Stabilność obrazu przy dużym obciążeniu pamięci PSRAM (ekran ładowania)

## Uwagi

Projekt hobbystyczny, bez związku z BMW AG. Nazwy BMW, xDrive itp. są znakami towarowymi ich właścicieli. Używasz na własną odpowiedzialność - urządzenie tylko czyta dane, ale podłączasz je do magistrali diagnostycznej auta.

Autor: **Diamel** - [diamel.pl](https://diamel.pl)
