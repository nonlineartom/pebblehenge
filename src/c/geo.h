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

/* Magnetic declination in degrees (true_heading = magnetic_heading + decl). */
float     geo_mag_declination_deg(void);
void      geo_set_mag_declination(float decl_deg);

/* Persistent storage I/O. geo_init() calls geo_load() automatically. */
void      geo_load(void);
void      geo_save(void);

#ifdef __cplusplus
}
#endif

#endif
