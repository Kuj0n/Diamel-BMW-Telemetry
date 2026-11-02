#include "ui_internal.h"
#include "psram.h"
#include <Arduino.h>
#include "can_test.h"
#include "catalog.h"
#include "comm.h"
#include "did_scan.h"
#include "display.h"
#include "settings.h"
#include "telemetry.h"

// --- Strona Serwis: narzędzia diagnostyczne połączenia z autem (tylko odczyt) ---
static lv_obj_t *lbl_can;
static lv_obj_t *btn_can;
static lv_obj_t *btn_car;
static lv_obj_t *lbl_car;
static lv_obj_t *btn_scan;
static lv_obj_t *lbl_scan;

static void add_section(lv_obj_t *list, const char *title) {
    lv_obj_t *l = make_label(list, &lv_font_montserrat_16, COLOR_ACCENT, title);
    lv_obj_set_style_pad_top(l, 12, 0);
}

static lv_obj_t *add_text(lv_obj_t *list, lv_color_t color) {
    lv_obj_t *l = make_label(list, &lv_font_montserrat_16, color, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static void run_can_test(lv_timer_t *) {
    CanTestResult r;
    bool ok = can_self_test(r);
    if (!r.driver_ok) {
        lv_label_set_text(lbl_can, "BLAD: nie udalo sie uruchomic kontrolera CAN.");
        lv_obj_set_style_text_color(lbl_can, COLOR_REC_ON, 0);
    } else if (ok) {
        lv_label_set_text_fmt(lbl_can, "OK: odebrano %d/%d ramek.\nTX (IO40), transceiver i RX (IO2) dzialaja.",
                              r.received, r.sent);
        lv_obj_set_style_text_color(lbl_can, lv_color_hex(0x81C784), 0);
    } else {
        lv_label_set_text_fmt(lbl_can,
                              "BLAD: odebrano %d/%d ramek (bledy TX %u, RX %u%s).\n"
                              "Sprawdz: zasilanie 3.3V transceivera (S3), przewody TX/RX, "
                              "czy TX/RX nie sa zamienione. Bez auta pomaga tez tymczasowy "
                              "rezystor 120 Ohm miedzy CANH i CANL.",
                              r.received, r.sent, (unsigned)r.tx_errors, (unsigned)r.rx_errors,
                              r.bus_off ? ", bus off" : "");
        lv_obj_set_style_text_color(lbl_can, COLOR_REC_ON, 0);
    }
    lv_obj_clear_state(btn_can, LV_STATE_DISABLED);
}

static void can_test_cb(lv_event_t *) {
    // Test potrzebuje kontrolera CAN na wyłączność (i nie wolno go robić w aucie)
    if (!uds_release()) {
        ui_toast("Najpierw zatrzymaj komunikacje (Menu > Stop)");
        return;
    }
    lv_obj_add_state(btn_can, LV_STATE_DISABLED);
    lv_label_set_text(lbl_can, "Test w toku...");
    lv_obj_set_style_text_color(lbl_can, COLOR_DIM, 0);
    // Test blokuje na ~1 s - uruchamiamy po odświeżeniu ekranu, żeby napis zdążył się pokazać
    lv_timer_t *t = lv_timer_create(run_can_test, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

#define CAR_PARAM_TIMEOUT_MS 500

// Test w aucie: nasłuch magistrali + jedno zapytanie odczytu do DDE; wynik słowami i surowe ramki
static void run_car_test(lv_timer_t *) {
    CanCarResult r;
    bool ok = can_car_test(r);
    const size_t BUF_LEN = 4096;
    static char *buf = psram_alloc<char>(BUF_LEN);
    size_t o = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (o < BUF_LEN) o += snprintf(buf + o, BUF_LEN - o, fmt, args...);
    };
    if (!r.driver_ok) {
        add("BLAD: nie udalo sie uruchomic kontrolera CAN.");
    } else {
        // 1) Nasłuch
        if (r.listen_frames) {
            add("Nasluch: %d ramek, %d ID (", r.listen_frames, r.listen_ids);
            for (int i = 0; i < r.listen_ids && i < CAR_TEST_IDS; i++) add("%s%03X", i ? " " : "", (unsigned)r.ids[i]);
            add(r.listen_ids > CAR_TEST_IDS ? " ...)\n" : ")\n");
        } else {
            add("Nasluch: cisza (BMW D-CAN milczy bez testera - to normalne)\n");
        }
        // Pojedynczy błąd = nasłuch włączony w środku ramki (magistrala zajęta powtórkami) - bez znaczenia
        if (r.listen_errors > 2) add("Bledy odbioru: %u - zamienione CANH/CANL, brak masy (pin 5) albo zaklocenia\n", (unsigned)r.listen_errors);
        // 2) Czy bramka słyszy nasze potwierdzenia (ACK)
        if (r.listen_frames > 1000) {
            if (r.normal_frames < r.listen_frames / 10) {
                add("Potwierdzenia: bramka nas SLYSZY (po ACK %d ramek zamiast %d) - nadawanie dziala.\n",
                    r.normal_frames, r.listen_frames);
            } else {
                add("Potwierdzenia: bramka NIE slyszy naszych ACK (%d ramek zamiast kilkunastu) - sygnal z modulu "
                    "nie dociera do auta.\n", r.normal_frames);
            }
        } else {
            add("Zwykly tryb: %d ramek w 1 s\n", r.normal_frames);
        }
        add("Proby: %d, przegrany arbitraz: %u, bledy magistrali: %u\n", r.attempts, (unsigned)r.arb_lost,
            (unsigned)r.bus_errors);
        // 3) Zapytanie do DDE
        if (!r.tx_acked && r.err_valid) {
            if (r.err_type == 3 && (r.err_seg == 0x19 || r.err_seg == 0x1B)) {
                add("Rodzaj bledu: brak potwierdzenia (ACK) - sygnal wychodzi poprawnie, ale nikt go nie odbiera.\n");
            } else if (r.err_type == 0) {
                add("Rodzaj bledu: BLAD BITU (miejsce 0x%02X) - nadany bit nie wraca z magistrali: zamienione CANH/CANL "
                    "albo modul nie wysterowuje magistrali auta.\n", r.err_seg);
            } else {
                add("Rodzaj bledu: %u, miejsce 0x%02X (%s)\n", r.err_type, r.err_seg, r.err_tx ? "nadawanie" : "odbior");
            }
        }
        if (!r.tx_acked) {
            add("Zapytanie: NIKT nie potwierdzil ramki (brak ACK).\nSprawdz: zaplon wlaczony, CANH = pin 6, CANL = pin 14, GND = pin 5.");
        } else if (r.dde_reply) {
            add("Zapytanie: DDE ODPOWIADA - polaczenie dziala.");
        } else {
            add("Zapytanie: ramka potwierdzona, ale DDE nie odpowiedzial (%d innych ramek).\nCzy zaplon jest wlaczony?", r.reply_count);
        }
        if (r.tx_errors || r.rx_errors || r.bus_off) {
            add("\nLiczniki bledow: TX %u, RX %u%s", (unsigned)r.tx_errors, (unsigned)r.rx_errors, r.bus_off ? ", BUS OFF" : "");
        }
        for (int i = 0; i < r.frame_count; i++) {
            add("\n%03X: %02X %02X %02X %02X %02X %02X %02X %02X", (unsigned)r.frame_id[i], r.frame_data[i][0],
                r.frame_data[i][1], r.frame_data[i][2], r.frame_data[i][3], r.frame_data[i][4], r.frame_data[i][5],
                r.frame_data[i][6], r.frame_data[i][7]);
        }
    }
    // 4) Po jednym parametrze z każdego modułu (najlepiej z listy podstawowej). Odmowa "poza zakresem"
    //    (0x31) -> próba w sesji rozszerzonej (10 03), potem powrót do zwykłej (10 01). Nic nie zapisuje.
    if (r.tx_acked) {
        static uint8_t resp[64];
        int answered = 0, refused = 0, silent = 0, ext_ok = 0;
        auto ask = [&](const ParamDef &pd, size_t &len, unsigned &dt) {
            uint8_t req[3] = {0x22, (uint8_t)(pd.did >> 8), (uint8_t)pd.did};
            len = 0;
            uint32_t t0 = millis();
            UdsResult u = uds_request(pd.addr, req, 3, resp, sizeof(resp), &len, CAR_PARAM_TIMEOUT_MS);
            dt = (unsigned)(millis() - t0);
            return u;
        };
        auto show = [&](UdsResult u, size_t len, unsigned dt) {
            if (u == UDS_OK || u == UDS_NEGATIVE) {
                for (size_t b = 0; b < len && b < 6; b++) add("%02X ", resp[b]);
                add("(%u ms)", dt);
            } else {
                add(u == UDS_NO_REPLY ? "brak odpowiedzi" : "%s", uds_result_text(u));
            }
        };
        add("\nModuly (1 parametr z kazdego):");
        for (int m = 1; m < modules_count(); m++) {
            const ModuleDef &md = module_def(m);
            if (!md.count) continue;
            int pick = md.first;
            for (int i = md.first; i < md.first + md.count; i++) {
                if (param(i).basic) { pick = i; break; }
            }
            const ParamDef &pd = param(pick);
            size_t len;
            unsigned dt;
            UdsResult u = ask(pd, len, dt);
            add("\n%02X %.14s %04X: ", md.addr, md.name, pd.did);
            show(u, len, dt);
            Serial.printf("Test modulu %02X %04X: wynik %d, %u B, %u ms\n", md.addr, pd.did, u, (unsigned)len, dt);
            if (u == UDS_OK) {
                answered++;
            } else if (u == UDS_NEGATIVE && len >= 3 && resp[2] == 0x31) {
                refused++;
                // Odczyt przez tymczasowy zestaw F303 (tak czyta zadanie BMW STATUS_BLOCK_LESEN):
                // 2C 03 F3 00 wyczyść, 2C 01 F3 00 [DID, pozycja 1, rozmiar] zdefiniuj, 22 F3 03 odczytaj
                auto ddli = [&](const char *label) {
                    static const uint8_t SIZES[] = {1, 1, 2, 2, 4, 4, 4};
                    const uint8_t clr[4] = {0x2C, 0x03, 0xF3, 0x03};
                    const uint8_t def[8] = {0x2C, 0x01, 0xF3, 0x03, (uint8_t)(pd.did >> 8), (uint8_t)pd.did, 1,
                                            (uint8_t)(pd.pos + SIZES[pd.type])};
                    const uint8_t rd[3] = {0x22, 0xF3, 0x03};
                    size_t l = 0;
                    unsigned t;
                    add("\n   %s 2C 01: ", label);
                    uds_request(md.addr, clr, 4, resp, sizeof(resp), &l, CAR_PARAM_TIMEOUT_MS);
                    uint32_t t0 = millis();
                    UdsResult d = uds_request(md.addr, def, 8, resp, sizeof(resp), &l, CAR_PARAM_TIMEOUT_MS);
                    show(d, l, (unsigned)(millis() - t0));
                    bool good = false;
                    if (d == UDS_OK) {
                        t0 = millis();
                        d = uds_request(md.addr, rd, 3, resp, sizeof(resp), &l, CAR_PARAM_TIMEOUT_MS);
                        t = (unsigned)(millis() - t0);
                        add("\n   22 F3 03: ");
                        show(d, l, t);
                        good = d == UDS_OK;
                    }
                    uds_request(md.addr, clr, 4, resp, sizeof(resp), &l, CAR_PARAM_TIMEOUT_MS);
                    return good;
                };
                bool good = ddli("zwykla sesja");
                if (!good) {
                    const uint8_t ext[2] = {0x10, 0x03};
                    size_t sl = 0;
                    if (uds_request(md.addr, ext, 2, resp, sizeof(resp), &sl, CAR_PARAM_TIMEOUT_MS) == UDS_OK) {
                        good = ddli("sesja 10 03");
                    } else {
                        add("\n   10 03 odrzucone");
                    }
                    const uint8_t back[2] = {0x10, 0x01};
                    uds_request(md.addr, back, 2, resp, sizeof(resp), &sl, CAR_PARAM_TIMEOUT_MS);
                }
                if (good) ext_ok++;
            } else if (u == UDS_NEGATIVE) {
                refused++;
            } else {
                silent++;
            }
        }
        add("\nPodsumowanie: odpowiada %d, odmowa %d (w tym dziala przez zestaw F303: %d), cisza %d", answered, refused,
            ext_ok, silent);
        uds_release();

        // 5) Standardowe OBD-II (tryb 01) - wartości silnika wymagane przepisami, niezależnie od BMW
        ObdResult ob;
        can_obd2_probe(ob);
        add("\nOBD-II (0x7DF, tryb 01):");
        for (int i = 0; i < OBD_PIDS; i++) {
            const uint8_t *d = ob.data[i];
            add("\n PID %02X: ", ob.pid[i]);
            if (!ob.ok[i]) {
                add("brak odpowiedzi");
                continue;
            }
            add("%03X ", (unsigned)ob.from[i]);
            switch (ob.pid[i]) {
                case 0x0C: add("obroty %u rpm", (unsigned)((d[0] << 8 | d[1]) / 4)); break;
                case 0x05: add("plyn chlodzacy %d C", d[0] - 40); break;
                case 0x0F: add("powietrze dolot %d C", d[0] - 40); break;
                case 0x0B: add("cisnienie dolot %u kPa", d[0]); break;
                case 0x42: add("napiecie %u mV", (unsigned)(d[0] << 8 | d[1])); break;
                default:   add("%02X %02X %02X %02X", d[0], d[1], d[2], d[3]); break;
            }
        }
    }
    lv_label_set_text(lbl_car, buf);
    lv_obj_set_style_text_color(lbl_car, ok ? lv_color_hex(0x81C784) : COLOR_REC_ON, 0);
    lv_obj_clear_state(btn_car, LV_STATE_DISABLED);
}

static void car_test_cb(lv_event_t *) {
    DidScanState s;
    did_scan_state(s);
    if (s.running) {
        ui_toast("Trwa skan parametrow - poczekaj albo go zatrzymaj");
        return;
    }
    if (!uds_release()) {
        ui_toast("Najpierw zatrzymaj komunikacje (Menu > Stop)");
        return;
    }
    lv_obj_add_state(btn_car, LV_STATE_DISABLED);
    lv_label_set_text(lbl_car, "Test w toku (do ok. 20 s - wszystkie moduly)...");
    lv_obj_set_style_text_color(lbl_car, COLOR_DIM, 0);
    lv_timer_t *t = lv_timer_create(run_car_test, 50, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// Skan wszystkich parametrów silnika (DDE) z katalogu - które numery sterownik naprawdę podaje
static void scan_cb(lv_event_t *e) {
    DidScanState s;
    did_scan_state(s);
    if (s.running) {
        did_scan_stop();
        return;
    }
    if (comm_running()) {
        ui_toast("Najpierw zatrzymaj komunikacje (Menu > Stop)");
        return;
    }
    bool block = lv_event_get_user_data(e) != NULL;
    if (!did_scan_start(0x12, block)) ui_toast("Nie mozna zaczac - brak karty SD albo katalogu DDE");
}

static void scan_refresh() {
    DidScanState s;
    did_scan_state(s);
    if (!s.file[0]) return;
    char line[240];
    snprintf(line, sizeof(line), "%s %d/%d: OK %d, odmowa %d, cisza %d%s%s\nWynik: Pliki > %s",
             s.running ? "Skan" : "Koniec", s.done, s.total, s.ok, s.refused, s.silent, s.note[0] ? "\nPRZERWANO: " : "",
             s.note, s.file);
    set_text_if_changed(lbl_scan, line);
    set_text_if_changed(lv_obj_get_child(btn_scan, 0), s.running ? "Zatrzymaj skan" : "Skan parametrow DDE");
}

// Obraz przesunięty (za duże obciążenie pamięci PSRAM) - ustawienie panelu od nowa, bez restartu
static void display_fix_cb(lv_event_t *) {
    display_redraw();
    ui_toast("Ekran przerysowany");
}

static lv_obj_t *sw_demo;

static void demo_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (on && comm_running()) comm_stop(); // symulacja zamiast auta - komunikacja niepotrzebna
    settings_set_demo(on);
    telemetry_start(); // uruchamia symulację; po wyłączeniu zadanie samo się kończy
    ui_toast(on ? "Tryb DEMO wlaczony - wartosci symulowane" : "Tryb DEMO wylaczony");
}

void service_page_build(lv_obj_t *page) {
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, SCREEN_W, PAGE_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 20, 0);
    lv_obj_set_style_pad_ver(list, 10, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    add_section(list, "TRYB DEMO");
    lv_obj_t *demo_row = plain_cont(list);
    lv_obj_set_size(demo_row, lv_pct(100), 50);
    lv_obj_t *demo_lbl = make_label(demo_row, &lv_font_montserrat_20, COLOR_TEXT, "Symulacja danych");
    lv_obj_align(demo_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    sw_demo = lv_switch_create(demo_row);
    lv_obj_align(sw_demo, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw_demo, demo_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *demo_hint = add_text(list, COLOR_DIM);
    lv_label_set_text(demo_hint, "Do testow bez auta: wartosci symulowane i przykladowe kody bledow. "
                                 "Wylaczone - zadnych wartosci DEMO na liscie parametrow i zadnej pracy w tle.");

    add_section(list, "MAGISTRALA CAN");
    lv_obj_t *can_info = add_text(list, COLOR_TEXT);
    lv_label_set_text_fmt(can_info, "Transceiver SN65HVD230, TX = IO%d, RX = IO%d, 500 kbps",
                          CAN_TX_PIN, CAN_RX_PIN);
    btn_car = make_button(list, "Test w aucie", car_test_cb, NULL);
    lv_obj_set_size(btn_car, 200, 46);
    lbl_car = add_text(list, COLOR_DIM);
    lv_label_set_text(lbl_car, "Wtyczka w gniezdzie OBD, zaplon wlaczony (silnik moze byc wylaczony). "
                               "Nasluch magistrali i zapytania odczytu do sterownikow - nic nie zapisuje.");
    btn_scan = make_button(list, "Skan parametrow DDE", scan_cb, NULL);
    lv_obj_set_size(btn_scan, 300, 46);
    lv_obj_t *btn_block = make_button(list, "Test zestawu DDE (F303)", scan_cb, (void *)1);
    lv_obj_set_size(btn_block, 300, 46);
    lv_obj_t *block_hint = add_text(list, COLOR_DIM);
    lv_label_set_text(block_hint, "Sprawdza kazdy pomiar silnika tak, jak czyta go odpytywanie (zestaw F303, "
                                  "grupy po 20). Wynik B12_....csv - lista pomiarow, ktore naprawde dzialaja.");
    lbl_scan = add_text(list, COLOR_DIM);
    lv_label_set_text(lbl_scan, "Pyta silnik o kazdy numer parametru z katalogu (tylko odczyt, ok. 1-2 min). "
                                "Wynik w pliku na karcie (Pliki / WWW).");
    btn_can = make_button(list, "Test CAN (bez auta)", can_test_cb, NULL);
    lv_obj_set_size(btn_can, 260, 46);
    lbl_can = add_text(list, COLOR_DIM);
    lv_label_set_text(lbl_can, "Test sprawdza tor TX -> transceiver -> RX bez auta.");
    lv_obj_t *warn = add_text(list, COLOR_ACCENT);
    lv_label_set_text(warn, "Test nadaje ramki - uruchamiaj tylko z wtyczka OBD odlaczona od auta.");

    add_section(list, "EKRAN");
    lv_obj_t *fix = make_button(list, LV_SYMBOL_REFRESH " Ustaw obraz", display_fix_cb, NULL);
    lv_obj_set_size(fix, 220, 46);
    lv_obj_t *fix_hint = add_text(list, COLOR_DIM);
    lv_label_set_text(fix_hint, "Gdy obraz sie przesunie: przerysowanie calego ekranu. Na kazdym ekranie dziala "
                                "tez przytrzymanie tytulu na pasku u gory, a samo dzieje sie przy zmianie strony.");
    lv_obj_set_style_pad_bottom(fix_hint, 20, 0);
}

void service_page_refresh() {
    scan_refresh();
    bool demo = g_settings.data_source == SRC_DEMO;
    if (lv_obj_has_state(sw_demo, LV_STATE_CHECKED) != demo) {
        if (demo) lv_obj_add_state(sw_demo, LV_STATE_CHECKED);
        else lv_obj_clear_state(sw_demo, LV_STATE_CHECKED);
    }
}
