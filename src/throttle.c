#include "throttle.h"
#include "util.h"
#include <dirent.h>
#include <pthread.h>
#include <math.h>
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

/* ---------------- controller ----------------
 * EMA-filtered temperature feeding a velocity-form PI loop. Velocity form has no
 * integrator state to wind up, and small gains keep several nodes sharing one
 * chip from fighting each other. Above the cap it multiplies duty down; at the
 * hard limit it stops outright. */
#define FILTER_TAU 2.0     /* s */
#define KP         0.6     /* % duty per °C of error change */
#define KI         0.15    /* % duty per °C·s of error */

double ctl_step(ctl_t *c, const ctl_cfg *k, double t, double dt) {
    if (!c->init) {
        c->ema = t; c->duty = k->duty_max * 0.25; c->prev_err = k->target - t; c->init = 1;
    }
    c->ema += (1.0 - exp(-dt / FILTER_TAU)) * (t - c->ema);
    if (t >= k->hard) { c->duty = 0; c->prev_err = k->target - c->ema; return 0; }
    double e = k->target - c->ema;
    c->duty += KP * (e - c->prev_err) + KI * e * dt;
    c->prev_err = e;
    if (t >= k->cap) c->duty *= 0.7;
    if (c->duty < 0) c->duty = 0;
    if (c->duty > k->duty_max) c->duty = k->duty_max;
    return c->duty;
}

/* ---------------- sensors ---------------- */
#define MAX_SENS 16
static char sens[MAX_SENS][200];
static int nsens;
static char sens_name[48] = "none";
static ctl_cfg cfg;
static ctl_t ctl;
static int batt_pause = 1, has_batt;
static atomic_int duty = 0, temp_now = -1, reason = TH_RUN;

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *r = fgets(buf, (int)n, f);
    fclose(f);
    if (!r) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int is_cpu_zone(const char *t) {
    return !strcmp(t, "x86_pkg_temp") || strstr(t, "coretemp") || strstr(t, "k10temp") ||
           strstr(t, "cpu") || strstr(t, "soc") || strstr(t, "tctl");
}

static int not_cpu_zone(const char *t) {
    return !strncmp(t, "pch", 3) || strstr(t, "iwlwifi") || strstr(t, "wifi") || !strncmp(t, "BAT", 3) ||
           strstr(t, "battery") || strstr(t, "nvme") || strstr(t, "gpu") || strstr(t, "amdgpu");
}

static void add_sensor(const char *path, const char *name) {
    if (nsens >= MAX_SENS) return;
    snprintf(sens[nsens++], sizeof sens[0], "%s", path);
    if (nsens == 1) snprintf(sens_name, sizeof sens_name, "%s", name);
}

/* CPU sensors first (thermal zones by type, then hwmon by driver name); if none,
 * every zone that isn't obviously something else. Returns the critical trip in °C. */
static int discover(void) {
    char path[300], buf[64];
    int crit = 0;
    DIR *d = opendir("/sys/class/thermal");
    struct dirent *e;
    for (int pass = 0; pass < 2 && nsens == 0; pass++) {
        if (!d) break;
        rewinddir(d);
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "thermal_zone", 12)) continue;
            snprintf(path, sizeof path, "/sys/class/thermal/%s/type", e->d_name);
            if (read_line(path, buf, sizeof buf)) continue;
            if (pass == 0 ? !is_cpu_zone(buf) : not_cpu_zone(buf)) continue;
            snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", e->d_name);
            add_sensor(path, buf);
            for (int t = 0; t < 12; t++) {           /* critical trip point, if published */
                char tp[300], ty[32];
                snprintf(tp, sizeof tp, "/sys/class/thermal/%s/trip_point_%d_type", e->d_name, t);
                if (read_line(tp, ty, sizeof ty)) break;
                if (strcmp(ty, "critical")) continue;
                snprintf(tp, sizeof tp, "/sys/class/thermal/%s/trip_point_%d_temp", e->d_name, t);
                if (!read_line(tp, ty, sizeof ty) && atoi(ty) / 1000 > crit) crit = atoi(ty) / 1000;
            }
        }
        if (pass == 0 && nsens == 0) {                 /* hwmon: AMD k10temp, Intel coretemp */
            DIR *h = opendir("/sys/class/hwmon");
            while (h && (e = readdir(h))) {
                if (e->d_name[0] == '.') continue;
                snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
                if (read_line(path, buf, sizeof buf) || !is_cpu_zone(buf)) continue;
                snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
                add_sensor(path, buf);
                snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_crit", e->d_name);
                if (!read_line(path, buf, sizeof buf) && atoi(buf) / 1000 > crit) crit = atoi(buf) / 1000;
            }
            if (h) closedir(h);
        }
    }
    if (d) closedir(d);
    return crit;
}

static int read_temp(void) {
    int best = -1;
    char buf[32];
    for (int i = 0; i < nsens; i++)
        if (!read_line(sens[i], buf, sizeof buf)) {
            int v = atoi(buf);
            if (v > best && v < 150000) best = v;
        }
    return best;
}

static int read_battery(int *present) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    int have_mains = 0, mains_online = 0;
    struct dirent *e;
    char path[300], buf[32];
    *present = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
        if (read_line(path, buf, sizeof buf)) continue;
        if (!strcmp(buf, "Battery")) *present = 1;
        else if (!strcmp(buf, "Mains")) {
            have_mains = 1;
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", e->d_name);
            if (!read_line(path, buf, sizeof buf) && atoi(buf) == 1) mains_online = 1;
        }
    }
    closedir(d);
    return *present && have_mains && !mains_online;
}

void throttle_init(int dmax, int cap_c, int pause_batt) {
    int crit = discover();
    read_battery(&has_batt);
    batt_pause = pause_batt;
    if (cap_c <= 0) {                                   /* auto: 20 °C under critical */
        cap_c = crit > 0 ? crit - 20 : 80;
        if (cap_c < 60) cap_c = 60;
        if (cap_c > 85) cap_c = 85;
    }
    cfg.cap = cap_c;
    cfg.target = cap_c - 6;               /* margin for sensor spikes around the median */
    cfg.hard = crit > 0 && crit - 5 < cap_c + 10 ? crit - 5 : cap_c + 10;
    cfg.duty_max = dmax < 0 ? 0 : dmax > 100 ? 100 : dmax;
    memset(&ctl, 0, sizeof ctl);
    atomic_store(&duty, (int)(cfg.duty_max * 0.25));
}

double median(double *v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double x = v[j]; v[j] = v[j - 1]; v[j - 1] = x; }
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* Samples at 10 Hz; every 0.9-1.3 s (jittered so co-located nodes don't act in
 * step) feeds the median of the window to the controller. */
static void *sampler(void *arg) {
    (void)arg;
    unsigned seed = (unsigned)now_ns();
    double win[16];
    int n = 0;
    uint64_t last = now_ns(), next = last + 900000000ULL + (uint64_t)(rand_r(&seed) % 400) * 1000000ULL;
    for (;;) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        int t = read_temp();
        if (t >= 0 && n < 16) win[n++] = t / 1000.0;
        uint64_t now = now_ns();
        if (now < next) continue;
        double dt = (double)(now - last) / 1e9;
        last = now;
        next = now + 900000000ULL + (uint64_t)(rand_r(&seed) % 400) * 1000000ULL;
        int present;
        if (read_battery(&present) && batt_pause) {
            atomic_store(&duty, 0); atomic_store(&reason, TH_BATTERY); n = 0; continue;
        }
        if (n == 0) { atomic_store(&duty, (int)cfg.duty_max); atomic_store(&reason, TH_RUN); continue; }
        double m = median(win, n), d = ctl_step(&ctl, &cfg, m, dt);
        n = 0;
        atomic_store(&temp_now, (int)(m * 1000));
        atomic_store(&duty, (int)(d + 0.5));
        atomic_store(&reason, m >= cfg.hard ? TH_HOT : d < cfg.duty_max - 0.5 ? TH_THERMAL : TH_RUN);
    }
    return NULL;
}

void throttle_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, sampler, NULL) == 0) pthread_detach(th);
}

void throttle_lower_thread(void) {
    struct sched_param sp = {0};
    syscall(SYS_sched_setscheduler, 0, SCHED_IDLE, &sp);   /* musl stubs the libc wrapper */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
}

/* Duty-cycle periods are jittered 80–120 ms per thread so co-located workers
 * don't burst in phase. */
void throttle_tick(tstate *t) {
    if (!t->seed) t->seed = (uint32_t)(now_ns() ^ (uint64_t)syscall(SYS_gettid) * 2654435761u);
    if (!t->period_ns) t->period_ns = 100000000ULL;
    uint64_t now = now_ns();
    int d = atomic_load(&duty);
    if (d <= 0) {
        struct timespec ts = {0, 250000000 + (long)(rand_r(&t->seed) % 250000000)};
        nanosleep(&ts, NULL);
        t->period_start = now_ns();
        return;
    }
    uint64_t el = now - t->period_start;
    if (el >= t->period_ns) goto next;
    if (d < 100 && el >= t->period_ns * (uint64_t)d / 100) {
        struct timespec ts = {0, (long)(t->period_ns - el)};
        nanosleep(&ts, NULL);
        goto next;
    }
    return;
next:
    t->period_start = now_ns();
    t->period_ns = 80000000ULL + (uint64_t)(rand_r(&t->seed) % 40000000u);
}

int throttle_duty(void) { return atomic_load(&duty); }
int throttle_temp_c(void) { int t = atomic_load(&temp_now); return t < 0 ? -1 : t / 1000; }
int throttle_target_c(void) { return (int)cfg.target; }
int throttle_cap_c(void) { return (int)cfg.cap; }
void throttle_fixed(int d) { atomic_store(&duty, d); }
int throttle_reason(void) { return atomic_load(&reason); }
int throttle_has_battery(void) { return has_batt; }
const char *throttle_sensor(void) { return sens_name; }

const char *throttle_reason_str(void) {
    static const char *r[] = {"running", "thermal", "too hot, stopped", "on battery, paused"};
    return r[atomic_load(&reason)];
}
