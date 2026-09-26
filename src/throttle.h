/* Keeps the node polite: SCHED_IDLE workers, jittered duty cycling, a smoothed
 * proportional-integral thermal controller, and battery pause. */
#ifndef THROTTLE_H
#define THROTTLE_H
#include <stdint.h>

typedef struct { uint64_t period_start, period_ns; uint32_t seed; } tstate;

/* ---- pure controller (unit-tested against a thermal model) ---- */
typedef struct {
    double target;       /* °C to settle at */
    double cap;          /* above this, back off fast */
    double hard;         /* at or above this, stop */
    double duty_max;     /* % */
} ctl_cfg;

typedef struct { double ema, duty, prev_err; int init, hot_run; } ctl_t;

double ctl_step(ctl_t *c, const ctl_cfg *k, double temp_c, double dt);

/* ---- node integration ---- */
enum { TH_RUN, TH_THERMAL, TH_HOT, TH_BATTERY, TH_SENSOR };

/* Median of n samples: package sensors spike on every turbo burst, the median
 * tracks the heat that actually accumulates. Sorts in place. */
double median(double *v, int n);

void throttle_init(int duty_max, int temp_cap_c /* 0 = auto */, int pause_on_battery);
void throttle_start(void);             /* sampler thread: 10 Hz reads, ~1 Hz control */
void throttle_fixed(int duty);         /* benchmarks: pin duty, no sampler */
int  throttle_cap_c(void);
void throttle_lower_thread(void);
void throttle_tick(tstate *t);
int  throttle_duty(void);
int  throttle_temp_c(void);            /* -1 if unknown */
int  throttle_target_c(void);
int  throttle_reason(void);
const char *throttle_reason_str(void);
int  throttle_has_battery(void);
const char *throttle_sensor(void);
#endif
