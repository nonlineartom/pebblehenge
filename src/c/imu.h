#ifndef PEBBLEHENGE_IMU_H
#define PEBBLEHENGE_IMU_H

#include <pebble.h>
#include <stdbool.h>

typedef struct {
    /* Pitch (deg). 0 = watch held horizontally (face up); +90 = watch
     * held vertically with the screen facing the user; -90 = face down. */
    float pitch_deg;
    /* Roll (deg). 0 = watch upright; positive = tipped to the right. */
    float roll_deg;
    /* True when the IMU has settled enough that pitch/roll are meaningful.
     * The first few samples after subscribe are dropped to avoid showing
     * a jump from zero on the first redraw. */
    bool  ready;
} pbh_attitude_t;

typedef void (*pbh_imu_handler)(pbh_attitude_t att);

/* Subscribe to accelerometer samples; produces a smoothed attitude
 * estimate ~10x per second and forwards it to `handler`. */
void pbh_imu_init(pbh_imu_handler handler);
void pbh_imu_deinit(void);

#endif
