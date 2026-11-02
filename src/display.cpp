#include "display.h"
#include <esp_lcd_panel_rgb.h>
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "config.h"
#include "touch_gt911.h"

// Konfiguracja ekranu GUITION ESP32-4848S040 (ST7701, 480x480)
// wg profilu ESP32_4848S040_86BOX_GUITION z przykładów Arduino_GFX

// SPI programowe do wysłania komend startowych do sterownika ST7701
// (piny 47/48 są współdzielone z kartą SD - SD inicjalizujemy dopiero po ekranie)
static Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */,
    48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

// Równoległa magistrala RGB565 do przesyłania obrazu.
// Bounce buffer: DMA ekranu czyta z pamięci wewnętrznej zamiast bezpośrednio z PSRAM -
// bez tego przy intensywnym rysowaniu obraz "pływa" i migocze (za mała przepustowość PSRAM).
// 30 linii: więcej zapasu, gdy PSRAM jest zajęty (obiekty LVGL i katalog też są w PSRAM).
// Rozmiar ramki musi być parzystą wielokrotnością bufora: 480*480 / (2*480*30) = 8.
#define BOUNCE_BUFFER_LINES 30
// 10 MHz zamiast 12: ok. 35 klatek panelu na sekundę i o 1/6 mniej odczytów z PSRAM
#define PIXEL_CLOCK_HZ 10000000
static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18 /* DE */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11 /* R0 */, 12 /* R1 */, 13 /* R2 */, 14 /* R3 */, 0 /* R4 */,
    8 /* G0 */, 20 /* G1 */, 3 /* G2 */, 46 /* G3 */, 9 /* G4 */, 10 /* G5 */,
    4 /* B0 */, 5 /* B1 */, 6 /* B2 */, 7 /* B3 */, 15 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    0 /* pclk_active_neg */, PIXEL_CLOCK_HZ /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */, SCREEN_W * BOUNCE_BUFFER_LINES /* bounce_buffer_size_px */);

static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    SCREEN_W, SCREEN_H, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

// 48 linii = 46 KB RAM wewnętrznego (było 80 linii / 77 KB - za mało zostawało dla Wi-Fi)
#define DRAW_BUFFER_LINES 48

// --- Ustawienie obrazu od nowa ---
// Przy dużym obciążeniu PSRAM transmisja do panelu RGB potrafi zgubić synchronizację i obraz zostaje
// przesunięty. esp_lcd_rgb_panel_restart() zaczyna transmisję od początku ramki (w przerwie VSYNC,
// CONFIG_LCD_RGB_RESTART_IN_VSYNC) - niewidocznie. Uchwyt panelu jest prywatny w Arduino_GFX,
// więc odczytujemy go zgodnym ze standardem sposobem (jawna instancjacja szablonu).
template <typename Tag, typename Tag::type M>
struct PrivateAccess {
    friend typename Tag::type get(Tag) { return M; }
};
struct PanelHandleTag {
    typedef esp_lcd_panel_handle_t Arduino_ESP32RGBPanel::*type;
    friend type get(PanelHandleTag);
};
template struct PrivateAccess<PanelHandleTag, &Arduino_ESP32RGBPanel::_panel_handle>;

void display_resync() {
    esp_lcd_panel_handle_t h = rgbpanel->*get(PanelHandleTag());
    if (h) esp_lcd_rgb_panel_restart(h);
}

// Pełne przerysowanie: gdy przesunięta jest sama zawartość pamięci obrazu (nieruchomy ekran,
// np. menu, nigdy by się nie odświeżył). Wołać z zadania LVGL.
void display_redraw() {
    display_resync();
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
}

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;
static bool touch_ok = false;
static uint8_t brightness = 0;

// Licznik klatek: LVGL woła monitor_cb po każdym odświeżeniu ekranu
static volatile uint32_t frame_count = 0;

static void monitor_cb(lv_disp_drv_t *, uint32_t, uint32_t) {
    frame_count++;
}

uint16_t display_fps() {
    static uint32_t last_ms = 0, last_count = 0;
    uint32_t now = millis(), count = frame_count;
    uint32_t dt = now - last_ms;
    uint16_t fps = dt ? (uint16_t)((count - last_count) * 1000 / dt) : 0;
    last_ms = now;
    last_count = count;
    return fps;
}

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp);
}

// Uśpienie: podświetlenie zgaszone, dotyk tylko budzi (nie klika w to, co jest pod palcem)
static volatile bool sleeping = false;
static volatile bool wake_request = false;
static bool ignore_until_release = false;

void display_sleep(bool on) {
    sleeping = on;
    if (on) wake_request = false;
}

bool display_take_wake() {
    bool w = wake_request;
    wake_request = false;
    return w;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint16_t x, y;
    bool pressed = gt911_read(x, y);
    if (sleeping || ignore_until_release) {
        if (sleeping && pressed) {
            wake_request = true;
            ignore_until_release = true;
        }
        if (!pressed) ignore_until_release = false;
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // GT911 w tym ekranie zgłasza w konfiguracji 1024x600, ale współrzędne podaje
    // już w pikselach 480x480 - bez skalowania
    int32_t px = x;
    int32_t py = y;
#if TOUCH_SWAP_XY
    int32_t t = px; px = py; py = t;
#endif
#if TOUCH_INVERT_X
    px = SCREEN_W - 1 - px;
#endif
#if TOUCH_INVERT_Y
    py = SCREEN_H - 1 - py;
#endif
    data->point.x = constrain(px, 0, SCREEN_W - 1);
    data->point.y = constrain(py, 0, SCREEN_H - 1);
    data->state = LV_INDEV_STATE_PR;

#if TOUCH_DEBUG
    static uint32_t last_print = 0;
    if (millis() - last_print > 250) {
        last_print = millis();
        Serial.printf("Dotyk: surowe %u,%u -> ekran %d,%d\n", x, y, data->point.x, data->point.y);
    }
#endif
}

// Panel RGB rejestruje przerwanie (kopiowanie bounce buffera) na rdzeniu, który wywołał begin().
// Uruchamiamy go na rdzeniu 0, żeby nie zabierał czasu LVGL na rdzeniu 1.
static SemaphoreHandle_t gfx_init_done;
static bool gfx_ok = false;

static void gfx_begin_task(void *) {
    gfx_ok = gfx->begin();
    xSemaphoreGive(gfx_init_done);
    vTaskDelete(NULL);
}

void display_init() {
    // Podświetlenie wyłączone do czasu wyczyszczenia ekranu (bez migania śmieciami)
    ledcAttach(TFT_BL, 5000, 8);
    display_set_brightness(0);

    gfx_init_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(gfx_begin_task, "gfx_init", 4096, NULL, 5, NULL, 0);
    xSemaphoreTake(gfx_init_done, portMAX_DELAY);
    vSemaphoreDelete(gfx_init_done);

    if (!gfx_ok) {
        Serial.println("BLAD: Nie udalo sie zainicjalizowac ekranu!");
    } else {
        Serial.println("SUKCES: Ekran RGB zainicjalizowany.");
    }
    gfx->fillScreen(0x0000);
    display_set_brightness(255);

    lv_init();

    // Jeden duży bufor rysowania LVGL w szybkiej pamięci wewnętrznej (odciąża szynę PSRAM,
    // z której ekran stale pobiera obraz). Flush jest synchroniczny (kopiowanie do framebuffera),
    // więc drugi bufor nic nie daje - liczy się rozmiar, czyli mniej kawałków na klatkę.
    const size_t buf_px = SCREEN_W * DRAW_BUFFER_LINES;
    buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1) {
        Serial.println("BLAD: Brak pamieci na bufor LVGL!");
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, buf_px);
    Serial.printf("Bufor LVGL: %u linii, wolny RAM: %u KB\n",
                  DRAW_BUFFER_LINES, (unsigned)(ESP.getFreeHeap() / 1024));

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.monitor_cb = monitor_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    touch_ok = gt911_begin(TOUCH_SDA, TOUCH_SCL);
    if (!touch_ok) {
        Serial.println("BLAD: Nie znaleziono kontrolera dotyku GT911!");
    }
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
}

void display_set_brightness(uint8_t level) {
    brightness = level;
    ledcWrite(TFT_BL, level);
}

uint8_t display_get_brightness() { return brightness; }

bool display_touch_ok() { return touch_ok; }

const uint16_t *display_framebuffer() { return gfx->getFramebuffer(); }
