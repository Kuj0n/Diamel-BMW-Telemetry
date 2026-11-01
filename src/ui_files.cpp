#include "ui_internal.h"
#include <Arduino.h>
#include <SD.h>
#include "logger.h"

// --- Strona Pliki: logi CSV na karcie SD, usuwanie z potwierdzeniem ---
#define MAX_FILES 200
#define CONFIRM_COLOR lv_color_hex(0xB71C1C)

struct FileEntry {
    char name[24];
    uint32_t size;
};

static lv_obj_t *lbl_card;
static lv_obj_t *list;
static lv_obj_t *btn_delete_all;
static FileEntry files[MAX_FILES];
static int file_count;

static void reload();

// Przeładowanie listy kasuje przyciski - z ich własnych zdarzeń wołamy je dopiero po obsłudze zdarzenia
static void reload_async(void *) {
    reload();
}

// Usuwanie wymaga drugiego dotknięcia: pierwsze zmienia przycisk na czerwony "Na pewno?"
static bool confirm_step(lv_obj_t *btn, const char *confirm_text) {
    if (lv_obj_get_user_data(btn)) return true;
    lv_obj_set_user_data(btn, (void *)1);
    lv_obj_set_style_bg_color(btn, CONFIRM_COLOR, 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), confirm_text);
    return false;
}

static bool delete_file(const char *name) {
    if (strcmp(name, logger_current_file()) == 0) return false; // plik w trakcie zapisu
    char path[40];
    snprintf(path, sizeof(path), "%s/%.23s", LOG_DIR, name);
    return SD.remove(path);
}

static void delete_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    if (!confirm_step(btn, "?")) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!delete_file(files[i].name)) ui_toast("Nie mozna usunac pliku");
    lv_async_call(reload_async, NULL);
}

static void delete_all_cb(lv_event_t *e) {
    if (!confirm_step(lv_event_get_target(e), "Na pewno usunac wszystkie?")) return;
    int failed = 0;
    for (int i = 0; i < file_count; i++) {
        if (!delete_file(files[i].name)) failed++;
    }
    if (failed) ui_toast("Pominieto plik w trakcie zapisu");
    lv_async_call(reload_async, NULL);
}

static void refresh_cb(lv_event_t *) {
    lv_async_call(reload_async, NULL);
}

static int compare_desc(const void *a, const void *b) {
    return strcmp(((const FileEntry *)b)->name, ((const FileEntry *)a)->name); // najnowsze na górze
}

static void reload() {
    lv_obj_clean(list);
    file_count = 0;
    lv_obj_set_user_data(btn_delete_all, NULL);
    lv_obj_set_style_bg_color(btn_delete_all, COLOR_BTN, 0);
    lv_label_set_text(lv_obj_get_child(btn_delete_all, 0), LV_SYMBOL_TRASH " Usun wszystkie");

    if (!logger_sd_ok()) {
        lv_label_set_text(lbl_card, "Brak karty SD (wloz karte i uruchom ponownie)");
        lv_obj_add_flag(btn_delete_all, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    File dir = SD.open(LOG_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && file_count < MAX_FILES; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                strlcpy(files[file_count].name, f.name(), sizeof(files[file_count].name));
                files[file_count].size = f.size();
                file_count++;
            }
            f.close();
        }
        dir.close();
    }
    qsort(files, file_count, sizeof(FileEntry), compare_desc);

    uint64_t total = SD.totalBytes() / (1024 * 1024);
    uint64_t used = SD.usedBytes() / (1024 * 1024);
    lv_label_set_text_fmt(lbl_card, "Karta: wolne %u MB z %u MB, plikow: %d",
                          (unsigned)(total - used), (unsigned)total, file_count);

    if (file_count == 0) {
        make_label(list, &lv_font_montserrat_20, COLOR_DIM, "Brak logow. Nagrywanie: przycisk REC.");
        lv_obj_add_flag(btn_delete_all, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(btn_delete_all, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < file_count; i++) {
        lv_obj_t *row = plain_cont(list);
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);

        bool recording = strcmp(files[i].name, logger_current_file()) == 0;
        lv_obj_t *name = make_label(row, &lv_font_montserrat_20, recording ? COLOR_REC_ON : COLOR_TEXT, files[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *size = make_label(row, &lv_font_montserrat_16, COLOR_DIM, "");
        if (recording) lv_label_set_text(size, "zapis...");
        else if (files[i].size < 1024) lv_label_set_text_fmt(size, "%u B", (unsigned)files[i].size);
        else lv_label_set_text_fmt(size, "%u KB", (unsigned)(files[i].size / 1024));
        lv_obj_align(size, LV_ALIGN_RIGHT_MID, -70, 0);

        if (!recording) {
            lv_obj_t *del = make_button(row, LV_SYMBOL_TRASH, delete_cb, (void *)(intptr_t)i);
            lv_obj_set_size(del, 56, 40);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
}

void files_page_build(lv_obj_t *page) {
    lbl_card = make_label(page, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_width(lbl_card, 330);
    lv_label_set_long_mode(lbl_card, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_card, 12, 12);

    lv_obj_t *refresh = make_button(page, LV_SYMBOL_REFRESH, refresh_cb, NULL);
    lv_obj_set_size(refresh, 60, 44);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -10, 5);

    list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H - 55 - 60);
    lv_obj_set_pos(list, 0, 55);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    btn_delete_all = make_button(page, "", delete_all_cb, NULL);
    lv_obj_set_size(btn_delete_all, 330, 46);
    lv_obj_align(btn_delete_all, LV_ALIGN_BOTTOM_MID, 0, -7);
}

void files_page_open() {
    reload();
}
