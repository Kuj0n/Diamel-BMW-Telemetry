#include "catalog.h"
#include <Arduino.h>
#include <SD.h>
#include <algorithm>
#include <ctype.h>
#include "logger.h"
#include "telemetry.h"

#define LINE_LEN 768
#define POOL_BLOCK (64 * 1024)
#define MODULES_MAX 64
#define UNITS_MAX 160

struct TextTable {
    uint8_t addr;
    const char *name;
    uint32_t first;
    uint16_t count;
};
struct TextEntry {
    int32_t value;
    const char *text;
};

static ParamDef *params;
static int nparams, params_cap;
static ModuleDef modules[MODULES_MAX + 1];
static int nmodules;
static int16_t module_of_addr[256];
static TextTable *tables;
static int ntables, tables_cap;
static TextEntry *texts;
static int ntexts, texts_cap;

static CatalogStatus st;
static volatile uint32_t bytes_done, bytes_total; // postęp wczytywania
static volatile bool done;
static SemaphoreHandle_t done_sem;

// --- Pamięć w PSRAM: napisy w blokach, tablice powiększane ---
static char *pool_cur;
static size_t pool_left;

static const char *pool_str(const char *s) {
    size_t n = strlen(s) + 1;
    if (n > pool_left) {
        size_t size = std::max((size_t)POOL_BLOCK, n);
        pool_cur = (char *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pool_left = pool_cur ? size : 0;
        if (!pool_cur) return "";
    }
    char *p = pool_cur;
    memcpy(p, s, n);
    pool_cur += n;
    pool_left -= n;
    return p;
}

// Jednostki powtarzają się tysiące razy - jedna kopia każdej
static const char *units[UNITS_MAX];
static int nunits;

static const char *unit_str(const char *u) {
    for (int i = 0; i < nunits; i++) {
        if (strcmp(units[i], u) == 0) return units[i];
    }
    const char *s = pool_str(u);
    if (nunits < UNITS_MAX) units[nunits++] = s;
    return s;
}

template <typename T>
static bool push(T *&arr, int &n, int &cap, const T &item) {
    if (n == cap) {
        int nc = cap ? cap * 2 : 1024;
        void *p = heap_caps_realloc(arr, nc * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) return false;
        arr = (T *)p;
        cap = nc;
    }
    arr[n++] = item;
    return true;
}

// --- Czytanie plików CSV linia po linii ---
class LineReader {
  public:
    explicit LineReader(File &f) : f_(f) {}
    bool next(char *line, size_t max) {
        size_t n = 0;
        for (;;) {
            if (pos_ >= len_) {
                len_ = f_.read((uint8_t *)buf_, sizeof(buf_));
                pos_ = 0;
                if (len_ > 0) bytes_done += len_;
                if (len_ <= 0) {
                    line[n] = 0;
                    return n > 0;
                }
            }
            char c = buf_[pos_++];
            if (c == '\n') break;
            if (c != '\r' && n + 1 < max) line[n++] = c;
        }
        line[n] = 0;
        return true;
    }

  private:
    File &f_;
    char buf_[4096];
    int len_ = 0, pos_ = 0;
};

static int split(char *line, char **fields, int max) {
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == ';') {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    return n;
}

static File open_catalog(const char *name) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", CATALOG_DIR, name);
    return SD.open(path);
}

static int add_module(uint8_t addr, const char *sgbd, const char *name, uint16_t dtc) {
    if (module_of_addr[addr] >= 0) return module_of_addr[addr];
    if (nmodules > MODULES_MAX) return MODULE_DEMO;
    ModuleDef &m = modules[nmodules];
    m.name = pool_str(name);
    m.sgbd = pool_str(sgbd);
    m.addr = addr;
    m.first = 0;
    m.count = 0;
    m.dtc_count = dtc;
    m.basic_count = 0;
    module_of_addr[addr] = nmodules;
    return nmodules++;
}

static void load_modules() {
    File f = open_catalog("moduly.csv");
    if (!f) return;
    LineReader r(f);
    char line[LINE_LEN], *fl[8];
    r.next(line, sizeof(line)); // nagłówek
    while (r.next(line, sizeof(line))) {
        if (split(line, fl, 8) < 5) continue;
        add_module((uint8_t)strtoul(fl[0], nullptr, 16), fl[1], fl[2], (uint16_t)atoi(fl[4]));
    }
    f.close();
}

static void load_texts() {
    File f = open_catalog("teksty.csv");
    if (!f) return;
    LineReader r(f);
    char line[LINE_LEN], *fl[6];
    r.next(line, sizeof(line));
    while (r.next(line, sizeof(line))) {
        if (split(line, fl, 6) < 4) continue;
        uint8_t addr = (uint8_t)strtoul(fl[0], nullptr, 16);
        // Wiersze jednej tabeli są w pliku kolejno
        if (!ntables || tables[ntables - 1].addr != addr || strcmp(tables[ntables - 1].name, fl[1]) != 0) {
            TextTable t = {addr, pool_str(fl[1]), (uint32_t)ntexts, 0};
            if (!push(tables, ntables, tables_cap, t)) break;
        }
        TextEntry e = {(int32_t)strtol(fl[2], nullptr, 0), pool_str(fl[3])};
        if (!push(texts, ntexts, texts_cap, e)) break;
        tables[ntables - 1].count++;
    }
    f.close();
}

static int16_t find_table(uint8_t addr, const char *name) {
    if (!*name) return -1;
    for (int i = 0; i < ntables; i++) {
        if (tables[i].addr == addr && strcmp(tables[i].name, name) == 0) return i;
    }
    return -1;
}

static int parse_type(const char *t) {
    static const char *const NAMES[] = {"U8", "S8", "U16", "S16", "U32", "S32", "F32"};
    for (int i = 0; i < 7; i++) {
        if (strcmp(t, NAMES[i]) == 0) return i;
    }
    return -1; // tekst, bajty, F64 - urządzenie pokazuje tylko liczby
}

static void load_params() {
    File f = open_catalog("parametry.csv");
    if (!f) {
        strlcpy(st.error, "Brak pliku parametry.csv", sizeof(st.error));
        return;
    }
    // Tablica od razu na docelowy rozmiar (wiersz ma ok. 150 B) - bez wielokrotnego kopiowania setek KB w PSRAM,
    // które przeciążało szynę pamięci wspólną z ekranem (obraz przeskakiwał)
    int want = nparams + (int)(f.size() / 100) + 64;
    if (want > params_cap) {
        void *p = heap_caps_realloc(params, want * sizeof(ParamDef), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            params = (ParamDef *)p;
            params_cap = want;
        }
    }
    LineReader r(f);
    char line[LINE_LEN], *fl[16];
    r.next(line, sizeof(line));
    while (r.next(line, sizeof(line))) {
        if ((st.params & 127) == 0) vTaskDelay(1); // przerwa dla ekranu (wspólna szyna PSRAM)
        // adres;klucz;nazwa;did;typ;pozycja;maska;kolejnosc;mnoznik;przesuniecie;jednostka;miejsca;teksty;opis
        int n = split(line, fl, 16);
        if (n < 13) continue;
        int type = parse_type(fl[4]);
        if (type < 0) {
            st.skipped++;
            continue;
        }
        uint8_t addr = (uint8_t)strtoul(fl[0], nullptr, 16);
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "Modul 0x%02X", addr);
        ParamDef d;
        d.name = pool_str(fl[2]);
        d.unit = unit_str(fl[10]);
        d.key = pool_str(fl[1]);
        d.factor = strtof(fl[8], nullptr);
        d.offset = strtof(fl[9], nullptr);
        d.mask = (uint32_t)strtoul(fl[6], nullptr, 0);
        d.did = (uint16_t)strtoul(fl[3], nullptr, 16);
        d.pos = (uint16_t)atoi(fl[5]);
        d.texts = find_table(addr, fl[12]);
        d.addr = addr;
        d.module = (uint8_t)add_module(addr, "", fallback, 0);
        d.type = (uint8_t)type;
        d.decimals = (uint8_t)constrain(atoi(fl[11]), 0, 6);
        d.big_endian = strcmp(fl[7], "LE") != 0;
        d.basic = n < 15 || atoi(fl[14]) == 1; // starszy katalog bez kolumny "poziom": wszystko
        if (!push(params, nparams, params_cap, d)) {
            strlcpy(st.error, "Za malo pamieci PSRAM", sizeof(st.error));
            break;
        }
        st.params++;
    }
    f.close();
}

static void load_task(void *) {
    uint32_t t0 = millis();
    if (!logger_sd_ok()) {
        strlcpy(st.error, "Brak karty SD", sizeof(st.error));
    } else if (!SD.exists(CATALOG_DIR)) {
        strlcpy(st.error, "Brak katalogu /diamel na karcie", sizeof(st.error));
    } else {
        static const char *const FILES[] = {"moduly.csv", "teksty.csv", "parametry.csv"};
        for (const char *name : FILES) {
            File f = open_catalog(name);
            if (f) {
                bytes_total += f.size();
                f.close();
            }
        }
        load_modules();
        load_texts();
        load_params();
        // Parametry jednego modułu obok siebie (wyszukiwanie w module = zakres indeksów); DEMO zostaje na początku
        // Plik jest zwykle już ułożony po adresie - sortowanie (ciężkie dla PSRAM) tylko w razie potrzeby
        auto by_module = [](const ParamDef &a, const ParamDef &b) { return a.module < b.module; };
        if (!std::is_sorted(params + P_COUNT, params + nparams, by_module)) {
            std::stable_sort(params + P_COUNT, params + nparams, by_module);
        }
        for (int i = P_COUNT; i < nparams; i++) {
            ModuleDef &m = modules[params[i].module];
            if (!m.count) m.first = i;
            m.count++;
            m.basic_count += params[i].basic;
        }
        for (int m = 1; m < nmodules; m++) {
            char name[24];
            snprintf(name, sizeof(name), "bledy_%02X.csv", modules[m].addr);
            File f = open_catalog(name);
            if (f) {
                st.dtc_files++;
                f.close();
            }
        }
        st.modules = nmodules - 1;
        st.text_tables = ntables;
        st.texts = ntexts;
        st.loaded = st.params > 0;
    }
    st.load_ms = millis() - t0;
    Serial.printf("Katalog: modulow %u, parametrow %u (pominietych %u), opisow %u, plikow bledow %u, %u ms%s%s\n",
                  st.modules, (unsigned)st.params, (unsigned)st.skipped, (unsigned)st.texts, st.dtc_files,
                  (unsigned)st.load_ms, st.error[0] ? ", " : "", st.error);
    st.done = true;
    done = true;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

void catalog_begin() {
    memset(module_of_addr, 0xFF, sizeof(module_of_addr));
    // Moduł 0: wbudowane wartości DEMO - indeksy jak w ParamId
    modules[0] = {"DEMO (symulacja)", "", 0, 0, P_COUNT, 0, P_COUNT};
    nmodules = 1;
    for (int i = 0; i < P_COUNT; i++) {
        ParamDef d = {PARAM_INFO[i].name, PARAM_INFO[i].unit, "", 1, 0, 0, 0, 0, -1, 0, MODULE_DEMO, PTYPE_F32,
                      PARAM_INFO[i].decimals, 1, 1};
        push(params, nparams, params_cap, d);
    }
    done_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(load_task, "CATALOG", 12288, NULL, 1, NULL, 0);
}

bool catalog_wait(uint32_t timeout_ms) {
    if (done) return true;
    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) xSemaphoreGive(done_sem);
    return done;
}

void catalog_status(CatalogStatus &out) { out = st; }

int catalog_progress() {
    if (done) return 100;
    return bytes_total ? (int)(bytes_done * 100ULL / bytes_total) : 0;
}

int params_count() { return done ? nparams : P_COUNT; }
const ParamDef &param(ParamIdx i) { return params[i]; }
int modules_count() { return done ? nmodules : 1; }
const ModuleDef &module_def(int m) { return modules[m]; }
int module_by_addr(uint8_t addr) { return module_of_addr[addr]; }

const char *param_value_text(ParamIdx i, float value) {
    int16_t t = params[i].texts;
    if (t < 0) return nullptr;
    int32_t v = lroundf(value);
    const TextTable &tb = tables[t];
    for (uint32_t k = tb.first; k < tb.first + tb.count; k++) {
        if (texts[k].value == v) return texts[k].text;
    }
    return nullptr;
}

static uint32_t key_hash(const char *s) {
    uint32_t h = 2166136261u; // FNV-1a
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h & 0xFFFFFF;
}

uint32_t param_ref(ParamIdx i) {
    const ParamDef &p = params[i];
    return p.addr ? ((uint32_t)p.addr << 24) | key_hash(p.key) : i;
}

void param_name_key(ParamIdx i, char *buf, size_t len) {
    if (i >= params_count()) buf[0] = 0;
    else if (!params[i].addr) snprintf(buf, len, "demo:%u", i);
    else snprintf(buf, len, "%02X:%s", params[i].addr, params[i].key);
}

ParamIdx param_by_name_key(const char *text) {
    if (!strncmp(text, "demo:", 5)) {
        int i = atoi(text + 5);
        return i >= 0 && i < P_COUNT ? (ParamIdx)i : PARAM_NONE;
    }
    const char *colon = strchr(text, ':');
    if (!colon || colon - text != 2) return PARAM_NONE;
    int m = module_of_addr[strtoul(text, nullptr, 16) & 0xFF];
    if (m < 0 || !done) return PARAM_NONE;
    for (int i = modules[m].first; i < modules[m].first + modules[m].count; i++) {
        if (!strcmp(params[i].key, colon + 1)) return i;
    }
    return PARAM_NONE;
}

ParamIdx param_by_ref(uint32_t ref) {
    if (!ref_is_catalog(ref)) return ref < P_COUNT ? (ParamIdx)ref : PARAM_NONE;
    int m = module_of_addr[ref >> 24];
    if (m < 0 || !done) return PARAM_NONE;
    for (int i = modules[m].first; i < modules[m].first + modules[m].count; i++) {
        if (param_ref(i) == ref) return i;
    }
    return PARAM_NONE;
}

static bool contains_ci(const char *text, const char *needle) {
    if (!*needle) return true;
    for (; *text; text++) {
        const char *t = text, *n = needle;
        while (*t && *n && tolower((unsigned char)*t) == tolower((unsigned char)*n)) {
            t++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

int params_search(int module, const char *query, int offset, ParamIdx *out, int max, bool all) {
    int from = 0, to = params_count();
    if (module >= 0 && module < modules_count()) {
        from = modules[module].first;
        to = from + modules[module].count;
    }
    int total = 0, n = 0;
    for (int i = from; i < to; i++) {
        if (!all && !params[i].basic) continue;
        if (!contains_ci(params[i].name, query) && !contains_ci(params[i].key, query)) continue;
        if (total >= offset && n < max) out[n++] = i;
        total++;
    }
    return total;
}

static File open_dtc_file(uint8_t addr) {
    char name[24];
    snprintf(name, sizeof(name), "bledy_%02X.csv", addr);
    return open_catalog(name);
}

void catalog_dtc_texts(uint8_t addr, const uint32_t *codes, int n, char (*out)[DTC_TEXT_LEN]) {
    for (int i = 0; i < n; i++) out[i][0] = 0;
    File f = open_dtc_file(addr);
    if (!f) return;
    LineReader r(f);
    char line[LINE_LEN], *fl[4];
    r.next(line, sizeof(line));
    while (r.next(line, sizeof(line))) {
        if (split(line, fl, 4) < 3) continue;
        uint32_t code = strtoul(fl[0], nullptr, 16);
        for (int i = 0; i < n; i++) {
            if (codes[i] == code && !out[i][0]) strlcpy(out[i], fl[2], DTC_TEXT_LEN);
        }
    }
    f.close();
}

int catalog_dtc_samples(uint8_t addr, uint32_t *codes, int max) {
    File f = open_dtc_file(addr);
    if (!f) return 0;
    LineReader r(f);
    char line[LINE_LEN], *fl[4];
    r.next(line, sizeof(line));
    int n = 0, row = 0;
    // Co 37. błąd od 15. - pierwsze wpisy to zwykle ogólne komunikaty (tryb oszczędzania energii itp.)
    while (n < max && r.next(line, sizeof(line))) {
        if (split(line, fl, 4) < 3 || strcmp(fl[1], "F") != 0) continue;
        if (row >= 15 && (row - 15) % 37 == 0) codes[n++] = strtoul(fl[0], nullptr, 16);
        row++;
    }
    f.close();
    return n;
}
