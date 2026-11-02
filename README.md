# Diamel BMW Telemetry

Wyświetlacz telemetrii do BMW serii F (testowane auto: **F31 320d xDrive, silnik B47, sterownik DDE D70BX7A0**), zbudowany na 4-calowym dotykowym panelu z ESP32-S3. Podłącza się do gniazda OBD, czyta na żywo wartości ze sterowników auta i pokazuje je na ekranie, na wykresie oraz w przeglądarce telefonu.

> Status: **wersja 0.15-test (wydanie testowe)** - nowe: wymuszenie regeneracji DPF, odczyt „regeneracja trwa”. Wersja 0.14 działa w aucie. Połączenie CAN, bieżące odczyty (w tym sterownik silnika), OBD-II i odczyt kodów błędów są sprawdzone w F31. Kafelek DPF i kasowanie błędów są nowe - jeszcze bez testu w aucie.

<!-- Zrzuty ekranu: docs/screenshots/ (zrób je funkcją "Zrzut ekranu" na stronie WWW urządzenia) -->

## Bezpieczeństwo: co urządzenie wysyła do auta

Urządzenie służy do **odczytu**. Lista dozwolonych usług jest zaszyta w kodzie, a każde inne zapytanie jest odrzucane przed wysłaniem na magistralę:

| Usługa | Do czego | Rodzaj |
|---|---|---|
| UDS `22` | odczyt wartości | odczyt |
| UDS `19` | odczyt kodów błędów | odczyt |
| UDS `2C 01` / `2C 03`, tylko zestawy `F300-F3FF` | tymczasowa lista pomiarów w RAM sterownika (tak czyta testery BMW); znika po wyłączeniu zapłonu | odczyt |
| UDS `10 01` / `10 03` | zwykła / rozszerzona sesja diagnostyczna (nigdy programowania) | - |
| UDS `3E` | podtrzymanie sesji | - |
| OBD-II tryb `01` | standardowe wartości silnika | odczyt |
| UDS `14 FF FF FF` | **kasowanie kodów błędów jednego modułu** - tylko na wyraźne polecenie (dwa dotknięcia przy wybranym module) | zapis |
| UDS `2E 5F B4` (tylko DDE) | **zgłoszenie regeneracji DPF**: ten sam rekord DPF odczytany ze sterownika, zmieniony wyłącznie licznik `TLRG` (jak zadanie BMW `ABGLEICH_CSF_PROG`, tryb 6); po zapisie urządzenie sprawdza, że nic innego się nie zmieniło. Warunki wypalania pilnuje DDE | zapis |

Nie ma kodowania, programowania ani innych zapisów do pamięci sterowników.

## Możliwości

- **3 strony odczytów** (przesuwane palcem), na każdej do 12 wartości (2 × 6), każda z własnym zestawem
- **Odpytywany jest tylko widoczny ekran** - strony, których nie oglądasz, nie obciążają magistrali; szybkość odczytu (cykle/s) widać na pasku
- **Sterownik silnika B47** czytany tak jak testery BMW: wszystkie wartości z ekranu w jednym zapytaniu (zestaw dynamiczny `F303`)
- **Standardowe OBD-II** (moduł „OBD-II (silnik)” w katalogu) - działa w każdym aucie z silnikiem spalinowym
- **Wykres** do 3 serii z automatyczną skalą (druga oś dla innej jednostki)
- **Kafelek DPF**: sadza (pomiar i model), popiół, różnica ciśnień, temperatura przed DPF, km i czas od regeneracji, liczba regeneracji, żądanie / blokada / przerwanie regeneracji
- **Kody błędów** na żądanie, z opisami; eksport do CSV; **kasowanie wybranego modułu**
- **Katalog ponad 10 000 parametrów** z 21 sterowników auta, w tym **lista podstawowa ~95 wartości z polskimi nazwami**
- **Log CSV na kartę microSD** - zapisuje ekran, na którym włączysz REC (strona odczytów albo wykres); do zatrzymania nagrywania ekran jest zablokowany
- **Wi-Fi**: własna sieć wyświetlacza (kod QR) albo sieć domowa (`http://diamel.local`); po restarcie wraca samo
- **Strona WWW**: podgląd na żywo, konfiguracja stron i wykresu, pliki, formatowanie karty, katalog, kopia ustawień (JSON), aktualizacja oprogramowania (OTA), restart, **zrzuty ekranu i nagrywanie wideo z ekranu**
- **Automatyczne usypianie** wg dowolnego parametru; dotyk budzi
- **Serwis** (Info → Serwis): test w aucie, skan parametrów, test zestawu F303, test CAN bez auta, tryb DEMO, przerysowanie ekranu
- **Tryb DEMO** z symulowanymi danymi - całkowicie wyłączany, bez DEMO nic nie działa w tle
- Zegar ustawiany z przeglądarki (opcjonalnie moduł RTC DS3231)

## Sprzęt

| Element | Opis |
|---|---|
| Wyświetlacz | **Guition / Sunton ESP32-4848S040**: 4.0" IPS 480×480 (ST7701, RGB), dotyk GT911, ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM), gniazdo microSD |
| Transceiver CAN | moduł **SN65HVD230** (3,3 V) - z rezystorem 120 Ω (patrz niżej) |
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
| GND | **pin 5** (masa sygnałowa) - koniecznie, nawet przy zasilaniu z USB |

CANH i CANL poprowadź skrętką. Pin 16 (+12 V) nie jest potrzebny.

**Rezystor 120 Ω (terminacja):** w F31 złącze OBD to osobna magistrala bramka ↔ tester, zakończona tylko po stronie auta - **adapter musi mieć rezystor 120 Ω** między CANH i CANL (R2 na module SN65HVD230 - zostaw go). W innym aucie zmierz rezystancję między pinami 6 i 14 przy wyłączonym zapłonie: ok. 120 Ω = rezystor potrzebny, ok. 60 Ω = usuń go.

Objaw braku rezystora albo złego połączenia: w *Teście w aucie* nikt nie potwierdza ramek, a w nasłuchu widać tylko ciągle powtarzaną ramkę `0x130` bramki.

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
- TWAI (CAN) + ISO-TP z adresowaniem rozszerzonym BMW (tester 0x6F1, odpowiedzi 0x600 + adres sterownika), OBD-II przez 0x7DF / 0x7E8
- ESPAsyncWebServer + ArduinoJson (strona WWW), NimBLE (adapter BLE)

### Kompilacja i wgranie

1. Zainstaluj VS Code + rozszerzenie PlatformIO.
2. Otwórz folder projektu, podłącz płytkę kablem USB-C (port z konwerterem CH340).
3. *PlatformIO: Upload* (pierwsza kompilacja trwa kilka minut - pobiera biblioteki).

Kolejne wersje możesz wgrywać bez kabla: strona WWW → *System* → *Aktualizacja oprogramowania* (plik `.pio/build/esp32-s3/firmware.bin` albo plik z [Releases](../../releases)).

### Pierwsze uruchomienie

1. Na ekranie: Menu → **Wi-Fi** → *Włącz*. Zeskanuj kod QR telefonem i otwórz adres z ekranu (`http://192.168.4.1`).
2. W zakładce **System** wgraj katalog parametrów (patrz niżej) i naciśnij *Uruchom ponownie*.
3. Podłącz wtyczkę, włącz zapłon, w menu naciśnij **Start**. Diagnoza połączenia: Info → Serwis → *Test w aucie*.

## Katalog parametrów

Opisy parametrów i błędów pochodzą z plików opisu sterowników BMW (`.prg`, format EDIABAS). **Te pliki są własnością BMW i nie są częścią projektu** - katalog generujesz ze swoich plików:

```
python tools/wyciagnij_f31.py <folder_z_plikami_prg> <folder_wyjsciowy>
```

Skrypt czyta tylko sterowniki z listy `CAR_ECUS` (dla innego auta zmień ją - nazwy plików SGBD pokaże np. identyfikacja pojazdu w BimmerDeepOBD), bierze adresy z `T_GRTB.PRG`, wyciąga parametry odczytu z tabel oraz - przez `tools/best_dekoder.py` - z kodu zadań diagnostycznych (np. skrzynia rozdzielcza xDrive), dokłada standardowe parametry OBD-II i tłumaczy nazwy na polski. Wynik to 4 pliki: `moduly.csv`, `parametry.csv`, `teksty.csv` i `bledy.csv`. Pierwsza linia każdego pliku zawiera wersję katalogu i datę - widać je w Info i na stronie WWW. Pliki wgrywasz przez stronę WWW: *System* → *Katalog parametrów*, potem *Uruchom ponownie*.

Sterownik silnika B47 odrzuca zwykły odczyt pomiarów (`7F 22 31`) - urządzenie samo przenosi takie parametry do zestawu dynamicznego `F303`. Które pomiary DDE naprawdę działają, pokaże Serwis → *Test zestawu DDE (F303)* (plik `B12_....csv`).

## Do zrobienia

- [x] Połączenie z autem (F31): CAN, bieżące odczyty (w tym DDE przez zestaw F303), OBD-II, kody błędów
- [x] Odpytywanie tylko widocznego ekranu, wskaźnik szybkości, log z bieżącego ekranu
- [ ] **Test w aucie**: kafelek DPF, kasowanie błędów, test zestawu F303 → lista działających pomiarów DDE w katalogu
- [ ] **Wymuszenie regeneracji DPF** - zrobione (kafelek DPF), do sprawdzenia w aucie
- [ ] **Odczyt przez adapter BLE (ELM327)** - wyszukiwanie, połączenie i test adaptera już są; brakuje zapytań UDS przez adapter (adapter z Bluetooth LE, np. vLinker MC+, OBDLink CX)
- [ ] **Zegar RTC DS3231** - obsługa jest, brak sprawdzenia na sprzęcie
- [ ] Procent spięcia xDrive, wartości min/max, progi ostrzegawcze
- [ ] Lepsze tłumaczenia pełnego katalogu (poza listą podstawową)
- [ ] Profile innych aut (OBD-II już działa w każdym aucie)
- [ ] Strona ze wskaźnikami (zegary analogowe)

## Licencja

[PolyForm Noncommercial 1.0.0](LICENSE.md) - © 2026 Diamel ([diamel.pl](https://diamel.pl)).

- **Wolno** (cele niekomercyjne): pobrać, zbudować urządzenie dla siebie, używać, uczyć się, modyfikować i dzielić się zmianami - z dołączeniem licencji i informacji o autorze.
- **Nie wolno**: używać komercyjnie - w szczególności sprzedawać oprogramowania ani urządzeń z nim, ani używać go w działalności zarobkowej.
- Licencja komercyjna - kontakt przez [diamel.pl](https://diamel.pl).

Wiążący jest angielski tekst licencji w [LICENSE.md](LICENSE.md).

## Uwagi

Projekt hobbystyczny, bez związku z BMW AG. Nazwy BMW, xDrive itp. są znakami towarowymi ich właścicieli. Używasz na własną odpowiedzialność - podłączasz urządzenie do magistrali diagnostycznej auta, a kasowanie błędów zmienia pamięć usterek sterownika.

Autor: **Diamel** - [diamel.pl](https://diamel.pl)
