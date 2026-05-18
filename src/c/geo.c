/* Phase 2: location is a build-time constant. Phase 3 will replace this
 * with a real fix received over AppMessage and persisted across launches. */

#include "geo.h"

/* Default seed location. Used until the phone supplies a real fix. */
#define DEFAULT_LAT_DEG        40.7128f
#define DEFAULT_LON_DEG       -74.0060f
#define DEFAULT_TZ_OFFSET_MIN  (-240)   /* EDT */

static geo_fix_t s_fix;

void geo_init(void) {
    s_fix.lat_deg       = DEFAULT_LAT_DEG;
    s_fix.lon_deg       = DEFAULT_LON_DEG;
    s_fix.tz_offset_min = DEFAULT_TZ_OFFSET_MIN;
    s_fix.fix_unix      = 0;
    s_fix.valid         = false;
}

geo_fix_t geo_current(void) {
    return s_fix;
}

void geo_set(float lat_deg, float lon_deg, int16_t tz_offset_min, int64_t fix_unix) {
    s_fix.lat_deg       = lat_deg;
    s_fix.lon_deg       = lon_deg;
    s_fix.tz_offset_min = tz_offset_min;
    s_fix.fix_unix      = fix_unix;
    s_fix.valid         = true;
}
