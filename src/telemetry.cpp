#include "telemetry.h"
#include <Arduino.h>
#include <math.h>

const char *const GROUP_NAMES[GROUP_COUNT] = {"Silnik", "xDrive", "Klima"};

const ParamInfo PARAM_INFO[P_COUNT] = {
    {"Obroty",            "rpm",  0, GROUP_ENGINE},
    {"Napiecie",          "V",    1, GROUP_ENGINE},
    {"Dolad. zadane",     "bar",  2, GROUP_ENGINE},
    {"Dolad. rzecz.",     "bar",  2, GROUP_ENGINE},
    {"Temp. dolotu",      "°C",   0, GROUP_ENGINE},
    {"Temp. spalin",      "°C",   0, GROUP_ENGINE},
    {"Temp. plynu",       "°C",   0, GROUP_ENGINE},
    {"Temp. oleju",       "°C",   0, GROUP_ENGINE},
    {"Sadza DPF",         "g",    1, GROUP_ENGINE},
    {"xDrive sprz.",      "%",    0, GROUP_XDRIVE},
    {"Cisn. klimy",       "bar",  1, GROUP_AC},
    {"Temp. parown.",     "°C",   1, GROUP_AC},
    {"Predkosc",          "km/h", 0, GROUP_ENGINE},
    {"Obciazenie",        "%",    0, GROUP_ENGINE},
    {"Pedal gazu",        "%",    0, GROUP_ENGINE},
    {"Moment rzecz.",     "Nm",   0, GROUP_ENGINE},
    {"Moment zadany",     "Nm",   0, GROUP_ENGINE},
    {"Dawka wtrysku",     "mg",   1, GROUP_ENGINE},
    {"Szyna zadane",      "bar",  0, GROUP_ENGINE},
    {"Szyna rzecz.",      "bar",  0, GROUP_ENGINE},
    {"Pow. zadane",       "mg",   0, GROUP_ENGINE},
    {"Pow. rzecz.",       "mg",   0, GROUP_ENGINE},
    {"Temp. za IC",       "°C",   0, GROUP_ENGINE},
    {"Temp. przed turbo", "°C",   0, GROUP_ENGINE},
    {"Temp. przed DPF",   "°C",   0, GROUP_ENGINE},
    {"Roznica cisn. DPF", "mbar", 0, GROUP_ENGINE},
    {"Regeneracja DPF",   "",     0, GROUP_ENGINE},
    {"Km od regeneracji", "km",   0, GROUP_ENGINE},
    {"Popiol DPF",        "g",    0, GROUP_ENGINE},
    {"Temp. paliwa",      "°C",   0, GROUP_ENGINE},
    {"Temp. zewn.",       "°C",   0, GROUP_ENGINE},
    {"Zawor EGR",         "%",    0, GROUP_ENGINE},
    {"Przepustnica",      "%",    0, GROUP_ENGINE},
    {"Lopatki VTG",       "%",    0, GROUP_ENGINE},
    {"Lambda",            "",     2, GROUP_ENGINE},
    {"Cisn. atm.",        "mbar", 0, GROUP_ENGINE},
    {"Cisn. oleju",       "bar",  1, GROUP_ENGINE},
    {"xDrive moment",     "Nm",   0, GROUP_XDRIVE},
    {"Temp. VTG",         "°C",   0, GROUP_XDRIVE},
    {"Temp. wnetrza",     "°C",   1, GROUP_AC},
    {"Sprezarka klimy",   "%",    0, GROUP_AC},
    {"Dmuchawa",          "%",    0, GROUP_AC},
};

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static TelemetrySnapshot current;

void telemetry_init() {
    memset(&current, 0, sizeof(current));
}

void telemetry_get(TelemetrySnapshot &out) {
    portENTER_CRITICAL(&mux);
    out = current;
    portEXIT_CRITICAL(&mux);
}

void telemetry_set(const TelemetrySnapshot &in) {
    portENTER_CRITICAL(&mux);
    current = in;
    portEXIT_CRITICAL(&mux);
}

// Symulacja jazdy: obciążenie zmienia się płynnie, rzeczywiste doładowanie
// dogania zadane z opóźnieniem (turbo lag), temperatury rosną do roboczych.
static void demo_task(void *) {
    float t = 0;
    float boost_act = 1.0f, rail_act = 300.0f, iat = 20.0f, egt = 150.0f;
    float coolant = 20.0f, oil = 20.0f, soot = 8.0f, km_regen = 120.0f;
    TelemetrySnapshot s;
    memset(&s, 0, sizeof(s));

    for (;;) {
        t += 0.05f;
        float load = 0.5f + 0.5f * sinf(t * 0.4f) * sinf(t * 0.13f + 1.0f); // 0..1

        // Wartości bazowe dla wszystkich parametrów, poniżej nadpisane realistycznymi przebiegami
        for (int i = 0; i < P_COUNT; i++) {
            s.value[i] = 50.0f + 10.0f * sinf(t * 0.3f + i);
            s.valid[i] = true;
        }

        float boost_tgt = 1.0f + load * 1.6f; // bar absolutne
        boost_act += (boost_tgt - boost_act) * 0.08f;
        float rail_tgt = 300.0f + load * 1500.0f;
        rail_act += (rail_tgt - rail_act) * 0.15f;
        iat += (25.0f + load * 20.0f - iat) * 0.01f;
        egt += (250.0f + load * 450.0f - egt) * 0.02f;
        coolant += (90.0f - coolant) * 0.002f;
        oil += (100.0f - oil) * 0.001f;
        soot += 0.0005f;
        km_regen += load * 0.0008f;
        float torque_tgt = 50.0f + load * 380.0f;

        s.value[P_RPM] = 800.0f + load * 3200.0f + 150.0f * sinf(t * 2.1f);
        s.value[P_BATTERY] = 14.2f + 0.1f * sinf(t * 0.5f);
        s.value[P_BOOST_TARGET] = boost_tgt;
        s.value[P_BOOST_ACTUAL] = boost_act;
        s.value[P_IAT] = iat;
        s.value[P_EGT] = egt;
        s.value[P_COOLANT] = coolant;
        s.value[P_OIL_TEMP] = oil;
        s.value[P_DPF_SOOT] = soot;
        s.value[P_XDRIVE_CLUTCH] = load * 40.0f + 10.0f * fabsf(sinf(t * 0.7f));
        s.value[P_AC_PRESSURE] = 12.0f + 2.0f * sinf(t * 0.3f);
        s.value[P_EVAP_TEMP] = 3.0f + 1.5f * sinf(t * 0.25f);
        s.value[P_SPEED] = 30.0f + load * 90.0f;
        s.value[P_LOAD] = load * 100.0f;
        s.value[P_PEDAL] = load * 85.0f;
        s.value[P_TORQUE_TARGET] = torque_tgt;
        s.value[P_TORQUE_ACTUAL] = torque_tgt * (boost_act / boost_tgt);
        s.value[P_INJ_QTY] = 5.0f + load * 45.0f;
        s.value[P_RAIL_TARGET] = rail_tgt;
        s.value[P_RAIL_ACTUAL] = rail_act;
        s.value[P_AIRMASS_TARGET] = 400.0f + load * 700.0f;
        s.value[P_AIRMASS_ACTUAL] = (400.0f + load * 700.0f) * (boost_act / boost_tgt);
        s.value[P_CAC_TEMP] = iat + 8.0f * load;
        s.value[P_EGT_PRE_TURBO] = egt + 120.0f;
        s.value[P_EGT_PRE_DPF] = egt - 40.0f;
        s.value[P_DPF_DIFF_PRESS] = 10.0f + load * 90.0f;
        s.value[P_DPF_REGEN] = 0.0f;
        s.value[P_DPF_KM_SINCE_REGEN] = km_regen;
        s.value[P_DPF_ASH] = 35.0f;
        s.value[P_AMBIENT_TEMP] = 12.0f;
        s.value[P_LAMBDA] = 3.0f - load * 1.8f;
        s.value[P_ATM_PRESS] = 1013.0f;
        s.value[P_OIL_PRESS] = 1.5f + load * 3.0f;
        s.value[P_XDRIVE_TORQUE] = load * 600.0f;
        s.updated_ms = millis();

        telemetry_set(s);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void telemetry_start_demo() {
    // Rdzeń 0 - tam docelowo pracuje komunikacja CAN.
    // Stos 8 KB: snapshot z 42 parametrami leży na stosie.
    xTaskCreatePinnedToCore(demo_task, "DEMO", 8192, NULL, 2, NULL, 0);
}
