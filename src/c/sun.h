#ifndef PEBBLEHENGE_SUN_H
#define PEBBLEHENGE_SUN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float azimuth;
    float altitude;
} sun_position_t;

typedef struct {
    int64_t solar_noon;
    int64_t sunrise;
    int64_t sunset;
    int64_t civil_dawn;
    int64_t civil_dusk;
    int64_t nautical_dawn;
    int64_t nautical_dusk;
    int64_t astronomical_dawn;
    int64_t astronomical_dusk;
    int64_t golden_hour_morning_end;
    int64_t golden_hour_evening_start;
    int64_t blue_hour_morning_end;
    int64_t blue_hour_evening_start;
} sun_day_events_t;

#define SUN_NEVER_RISES ((int64_t)INT64_MIN)
#define SUN_NEVER_SETS  ((int64_t)INT64_MAX)

sun_position_t sun_position(int64_t unix_utc, float lat_deg, float lon_deg);

int64_t sun_event(int64_t date_unix_utc, float lat_deg, float lon_deg,
                  float target_alt_deg, bool morning);

sun_day_events_t sun_day_events(int64_t unix_utc, float lat_deg, float lon_deg,
                                int tz_offset_min);

#ifdef __cplusplus
}
#endif

#endif
