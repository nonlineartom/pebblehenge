#ifndef PEBBLEHENGE_UI_ARC_H
#define PEBBLEHENGE_UI_ARC_H

#include <pebble.h>
#include <stdint.h>

#include "sun.h"
#include "geo.h"

/* Sample of the sun's position at a moment in the local day. */
typedef struct {
    int16_t minutes_from_local_midnight;  /* 0..1440 */
    float   altitude_deg;                 /* refraction-corrected */
    float   azimuth_deg;
} arc_sample_t;

#define ARC_SAMPLE_COUNT 49  /* every 30 minutes across the local day */

/* Recompute the day's arc samples and events for the given fix. Call
 * whenever the fix changes or the local day rolls over. */
void ui_arc_recompute(const geo_fix_t *fix);

/* The cached event set, in UTC unix time. Mirrors sun_day_events_t. */
const sun_day_events_t *ui_arc_events(void);

/* Set the "now" marker (UTC unix time). Triggers a layer redraw. */
void ui_arc_set_now(int64_t now_utc);

/* Set the observer's true heading in degrees (0..360). Triggers a redraw
 * so the sky arc centres on whatever direction the user is facing. Pass
 * a negative number to render in "north-up" absolute mode (used when the
 * compass is uncalibrated). */
void ui_arc_set_heading(float heading_deg);

/* Lifecycle for the canvas. The Layer's update_proc reads from the
 * cached samples plus current-now and renders bands + arc curve. */
Layer *ui_arc_layer_create(GRect frame);
void   ui_arc_layer_destroy(Layer *layer);

#endif
