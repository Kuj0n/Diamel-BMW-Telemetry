#pragma once
#include <esp_heap_caps.h>

// Większe bufory w PSRAM: wewnętrzny RAM (ok. 300 KB) potrzebuje Wi-Fi - przy kilku KB wolnego
// połączenie się zestawia, ale nie przechodzi żaden pakiet (nawet ping)
template <typename T>
static inline T *psram_alloc(size_t count) {
    return (T *)heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM);
}
