/* Compass wrapper: subscribes to Pebble's accel+mag-fused CompassService,
 * applies user-supplied magnetic declination to obtain true heading, and
 * forwards samples to a single app-level handler. */

#include "compass.h"

#define HEADING_FILTER_DEG  2

static pbh_compass_handler s_handler;
static float               s_decl_deg;

static float trig_to_deg(int32_t trig_angle) {
    /* TRIG_MAX_ANGLE = 65536. Normalize into [0,360). */
    float deg = (float)trig_angle * (360.0f / (float)TRIG_MAX_ANGLE);
    while (deg <    0.0f) deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;
    return deg;
}

static void on_heading(CompassHeadingData heading) {
    if (!s_handler) return;
    pbh_compass_t s;
    s.status     = heading.compass_status;
    s.calibrated = (heading.compass_status == CompassStatusCalibrated);
    float mag_deg = trig_to_deg(heading.magnetic_heading);
    float true_deg = mag_deg + s_decl_deg;
    while (true_deg <    0.0f) true_deg += 360.0f;
    while (true_deg >= 360.0f) true_deg -= 360.0f;
    s.heading_deg = true_deg;
    s_handler(s);
}

void pbh_compass_init(pbh_compass_handler handler, float decl_deg) {
    s_handler  = handler;
    s_decl_deg = decl_deg;
    compass_service_set_heading_filter(
        DEG_TO_TRIGANGLE(HEADING_FILTER_DEG));
    compass_service_subscribe(on_heading);
}

void pbh_compass_set_declination(float decl_deg) {
    s_decl_deg = decl_deg;
}

void pbh_compass_deinit(void) {
    compass_service_unsubscribe();
    s_handler = NULL;
}
