/* Keeps the node polite: SCHED_IDLE workers, duty cycling, thermal and battery backoff. */
#ifndef THROTTLE_H
#define THROTTLE_H
#include <stdint.h>

typedef struct { uint64_t period_start; } tstate;

void throttle_init(int duty_max, int temp_max_c, int pause_on_battery);
void throttle_update(void);            /* call every ~2s from the main loop */
void throttle_lower_thread(void);      /* call once at worker start */
void throttle_tick(tstate *t);         /* call often from workers; may sleep */
int  throttle_duty(void);
int  throttle_temp_c(void);            /* -1 if unknown */
int  throttle_on_battery(void);
#endif
