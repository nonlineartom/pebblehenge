#ifndef PEBBLEHENGE_GEO_H
#define PEBBLEHENGE_GEO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    lat_deg;
    float    lon_deg;
    int16_t  tz_offset_min;
    int64_t  fix_unix;        /* 0 = never fixed */
    bool     valid;
} geo_fix_t;

void      geo_init(void);
geo_fix_t geo_current(void);
void      geo_set(float lat_deg, float lon_deg, int16_t tz_offset_min, int64_t fix_unix);

#ifdef __cplusplus
}
#endif

#endif
