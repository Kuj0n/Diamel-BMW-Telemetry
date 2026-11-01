#pragma once
// Pamięć LVGL (obiekty, style, teksty) w PSRAM.
// Zwykły malloc daje bloki < 4 KB z wewnętrznego RAM - kilkaset obiektów interfejsu zjadało go
// w całości i brakowało miejsca dla systemu (fopen na karcie SD, start Wi-Fi -> abort()).
// Wewnętrzny RAM tylko awaryjnie, gdy PSRAM się skończy. Bufor rysowania nadal jest w RAM (display.cpp).
#include <esp_heap_caps.h>

static inline void *lv_psram_alloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void *lv_psram_realloc(void *p, size_t size) {
    return heap_caps_realloc_prefer(p, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void lv_psram_free(void *p) {
    heap_caps_free(p);
}
