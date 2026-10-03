// matrix_backend.c — maps a matrix_type_t to its backend vtable.

#include "matrix_backend.h"
#include "esp_log.h"

const matrix_backend_t *matrix_preview_backend(void);
const matrix_backend_t *matrix_max7219_backend(void);
const matrix_backend_t *matrix_hub75_backend(void);

const matrix_backend_t *matrix_backend_select(matrix_type_t type)
{
    switch (type) {
    case MX_HUB75:
#if CONFIG_SIP_MATRIX_HUB75
        return matrix_hub75_backend();
#else
        ESP_LOGW("MX_BE", "HUB75 requested but CONFIG_SIP_MATRIX_HUB75 is off; using preview");
        return matrix_preview_backend();
#endif
    case MX_MAX7219:
#if CONFIG_SIP_MATRIX_MAX7219
        return matrix_max7219_backend();
#else
        ESP_LOGW("MX_BE", "MAX7219 requested but CONFIG_SIP_MATRIX_MAX7219 is off; using preview");
        return matrix_preview_backend();
#endif
    case MX_PREVIEW:
    default:
        return matrix_preview_backend();
    }
}
