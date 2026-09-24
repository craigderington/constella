#include "throttle.h"
#include "util.h"
#include <dirent.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#define PERIOD_NS 100000000ULL          /* 100 ms duty-cycle period */

static int duty_max = 50, temp_max = 70000, batt_pause = 1;
static atomic_int duty = 50, temp_now = -1, on_batt = 0;

void throttle_init(int dmax, int tmax_c, int pause_batt) {
    duty_max = dmax < 0 ? 0 : dmax > 100 ? 100 : dmax;
    temp_max = tmax_c * 1000;
    batt_pause = pause_batt;
    atomic_store(&duty, duty_max);
}

void throttle_lower_thread(void) {
    /* musl stubs sched_setscheduler(), so go straight to the syscall. */
    struct sched_param sp = {0};
    syscall(SYS_sched_setscheduler, 0, SCHED_IDLE, &sp);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
}

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *r = fgets(buf, (int)n, f);
    fclose(f);
    if (!r) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int read_temp(void) {
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return -1;
    int best = -1;
    struct dirent *e;
    char path[300], buf[32];
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", e->d_name);
        if (read_line(path, buf, sizeof buf) == 0) {
            int v = atoi(buf);
            if (v > best && v < 150000) best = v;
        }
    }
    closedir(d);
    return best;
}

static int read_battery(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    int have_batt = 0, have_mains = 0, mains_online = 0;
    struct dirent *e;
    char path[300], buf[32];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
        if (read_line(path, buf, sizeof buf)) continue;
        if (!strcmp(buf, "Battery")) have_batt = 1;
        else if (!strcmp(buf, "Mains")) {
            have_mains = 1;
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", e->d_name);
            if (!read_line(path, buf, sizeof buf) && atoi(buf) == 1) mains_online = 1;
        }
    }
    closedir(d);
    return have_batt && have_mains && !mains_online;
}

void throttle_update(void) {
    int t = read_temp(), d = atomic_load(&duty), b = read_battery();
    atomic_store(&temp_now, t);
    atomic_store(&on_batt, b);
    if (b && batt_pause)                d = 0;
    else if (t < 0)                     d = duty_max;               /* no sensor: trust the cap */
    else if (t >= temp_max)             d = 0;                      /* hard stop */
    else if (t >= temp_max - 5000)      d = d > 20 ? d - 10 : 10;   /* approaching: back off */
    else if (t < temp_max - 10000)      d = d + 5 > duty_max ? duty_max : d + 5;
    atomic_store(&duty, d);
}

void throttle_tick(tstate *t) {
    uint64_t now = now_ns();
    int d = atomic_load(&duty);
    if (d <= 0) {
        struct timespec ts = {0, 500000000};
        nanosleep(&ts, NULL);
        t->period_start = now_ns();
        return;
    }
    uint64_t el = now - t->period_start;
    if (el >= PERIOD_NS) { t->period_start = now; return; }
    if (d < 100 && el >= PERIOD_NS * (uint64_t)d / 100) {
        uint64_t rest = PERIOD_NS - el;
        struct timespec ts = {0, (long)rest};
        nanosleep(&ts, NULL);
        t->period_start = now_ns();
    }
}

int throttle_duty(void) { return atomic_load(&duty); }
int throttle_temp_c(void) { int t = atomic_load(&temp_now); return t < 0 ? -1 : t / 1000; }
int throttle_on_battery(void) { return atomic_load(&on_batt); }
