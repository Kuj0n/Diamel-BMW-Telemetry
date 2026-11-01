#pragma once
#include <stdint.h>

// Katalog parametrów. Ustawienia zapisują numery parametrów, więc nowe pozycje
// dopisujemy WYŁĄCZNIE na końcu (przed P_COUNT), niczego nie przestawiamy ani nie usuwamy.
// Nazwy i zakresy są przykładowe (DEMO).
enum ParamId : uint16_t {
    P_RPM,
    P_BATTERY,
    P_BOOST_TARGET,
    P_BOOST_ACTUAL,
    P_IAT,
    P_EGT,
    P_COOLANT,
    P_OIL_TEMP,
    P_DPF_SOOT,
    P_XDRIVE_CLUTCH,
    P_AC_PRESSURE,
    P_EVAP_TEMP,
    P_SPEED,
    P_LOAD,
    P_PEDAL,
    P_TORQUE_ACTUAL,
    P_TORQUE_TARGET,
    P_INJ_QTY,
    P_RAIL_TARGET,
    P_RAIL_ACTUAL,
    P_AIRMASS_TARGET,
    P_AIRMASS_ACTUAL,
    P_CAC_TEMP,
    P_EGT_PRE_TURBO,
    P_EGT_PRE_DPF,
    P_DPF_DIFF_PRESS,
    P_DPF_REGEN,
    P_DPF_KM_SINCE_REGEN,
    P_DPF_ASH,
    P_FUEL_TEMP,
    P_AMBIENT_TEMP,
    P_EGR_POS,
    P_THROTTLE_POS,
    P_VTG_POS,
    P_LAMBDA,
    P_ATM_PRESS,
    P_OIL_PRESS,
    P_XDRIVE_TORQUE,
    P_XDRIVE_TEMP,
    P_AC_INTERIOR_TEMP,
    P_AC_COMPRESSOR,
    P_AC_BLOWER,
    P_COUNT
};

enum ParamGroup : uint8_t { GROUP_ENGINE, GROUP_XDRIVE, GROUP_AC, GROUP_COUNT };

extern const char *const GROUP_NAMES[GROUP_COUNT];

struct ParamInfo {
    const char *name; // bez polskich znaków - wbudowane czcionki LVGL ich nie mają
    const char *unit;
    uint8_t decimals;
    ParamGroup group;
};

extern const ParamInfo PARAM_INFO[P_COUNT];

struct TelemetrySnapshot {
    float value[P_COUNT];
    bool valid[P_COUNT];
    uint32_t updated_ms;
};

void telemetry_init();
// Bezpieczne między zadaniami (rdzeń CAN/DEMO zapisuje, rdzeń UI czyta)
void telemetry_get(TelemetrySnapshot &out);
void telemetry_set(const TelemetrySnapshot &in);
// Wartość parametru z rejestru (catalog.h); false = brak wartości. Bezpieczne między zadaniami.
bool telemetry_value(uint16_t idx, float &out);

// Zadanie danych: w trybie DEMO generuje symulację, w trybie auta (CAN)
// na razie oznacza wszystko jako brak wartości - odczyt z auta dojdzie w kolejnym etapie.
// Źródło (g_settings.data_source) można zmieniać w trakcie pracy.
void telemetry_start();
