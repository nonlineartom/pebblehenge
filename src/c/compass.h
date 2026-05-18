#ifndef PEBBLEHENGE_COMPASS_H
#define PEBBLEHENGE_COMPASS_H

#include <pebble.h>
#include <stdbool.h>

typedef struct {
    /* True heading in degrees, [0, 360). Magnetic heading + declination. */
    float    heading_deg;
    /* Raw status from the CompassService. */
    CompassStatus status;
    /* True if the last sample reflects a calibrated compass. */
    bool     calibrated;
} pbh_compass_t;

typedef void (*pbh_compass_handler)(pbh_compass_t sample);

/* Subscribe to compass updates. `decl_deg` is the magnetic declination
 * (true_heading = magnetic + decl); pass 0 if unknown. */
void pbh_compass_init(pbh_compass_handler handler, float decl_deg);

/* Update the declination if a fresher one arrives via AppMessage. */
void pbh_compass_set_declination(float decl_deg);

void pbh_compass_deinit(void);

#endif
