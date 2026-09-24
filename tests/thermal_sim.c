/* Closed-loop simulation of the throttle on a model of an i7-8850H laptop.
 *
 * Two-part plant fitted to observed behavior:
 *   sink: slow (tau 30 s) toward idle + 12 C per average busy core
 *   die:  sink + an instant spike while any core boosts (+12 C for the first
 *         core, +4 C per additional one). x86_pkg_temp reads the die.
 * That reproduces what was seen live: 93-97 C readings while nodes reported
 * duty 0, and a drop to ~68 C within seconds of stopping everything.
 * Workers duty-cycle in 80-120 ms periods; the browser adds random bursts. */
#include "throttle.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define IDLE    58.0
#define K_SINK  12.0
#define TAU_S   30.0
#define DT      0.005
#define SIM_T   900.0
#define SETTLE  300.0

typedef struct { double smin, smax, ssum, mmin, mmax, busy, zero; int n; double ms[1024]; int nm; } stats;

static unsigned rs;
static double urand(void) { rs = rs * 1103515245u + 12345u; return (rs >> 8) / 16777216.0; }

static double die(double sink, int active) { return active ? sink + 12 + 4 * (active - 1) : sink; }

/* Previous controller: 2 s updates on the raw reading, hard stop at cap. */
static double old_step(double d, double t, double cap, double dmax) {
    if (t >= cap) return 0;
    if (t >= cap - 5) return d > 20 ? d - 10 : 10;
    if (t < cap - 10) return d + 5 > dmax ? dmax : d + 5;
    return d;
}

typedef struct { double duty, pstart, plen, next_ctl, next_samp, win[16]; int nwin; ctl_t c; } node;

/* Mirrors throttle_init: target 6 C under the cap, hard stop 5 C under the
 * chip's critical when it publishes one, else 10 C over the cap. */
static ctl_cfg mkcfg(double cap, double crit, double dmax) {
    ctl_cfg k = {cap - 6, cap, crit > 0 && crit - 5 < cap + 10 ? crit - 5 : cap + 10, dmax};
    return k;
}

static stats run(int nodes, double dmax, int use_new, double cap, int browser, unsigned seed) {
    ctl_cfg k = mkcfg(cap, 0, dmax);
    node N[16];
    memset(N, 0, sizeof N);
    double sink = IDLE, bstart = 0, blen = 0;
    static stats s;
    memset(&s, 0, sizeof s);
    s.smin = s.mmin = 1e9; s.smax = s.mmax = -1e9;
    rs = seed;
    for (int i = 0; i < nodes; i++) {
        N[i].duty = dmax * 0.25; N[i].plen = 0.1; N[i].pstart = urand() * 0.1;
        N[i].next_ctl = use_new ? 0.9 + urand() * 0.4 : 2.0;
    }
    for (double t = 0; t < SIM_T; t += DT) {
        int active = 0;
        double mwin = 0;
        for (int i = 0; i < nodes; i++) {
            node *x = &N[i];
            if (t >= x->pstart + x->plen) { x->pstart = t; x->plen = 0.08 + urand() * 0.04; }
            active += (t - x->pstart) < x->plen * x->duty / 100.0;
        }
        if (browser && t >= bstart + 0.1) { bstart = t; blen = urand() < 0.05 ? 0.03 : 0; }
        int total = active + (t - bstart < blen);
        sink += (IDLE + K_SINK * total - sink) * (1 - exp(-DT / TAU_S));
        double T = die(sink, total);
        for (int i = 0; i < nodes; i++) {
            node *x = &N[i];
            double r = floor(T + (urand() * 2 - 1) + 0.5);
            if (use_new) {
                if (t >= x->next_samp) { x->next_samp = t + 0.1; if (x->nwin < 16) x->win[x->nwin++] = r; }
                if (t >= x->next_ctl && x->nwin) {
                    double m = median(x->win, x->nwin);
                    x->duty = ctl_step(&x->c, &k, m, 1.1);
                    x->nwin = 0; x->next_ctl = t + 0.9 + urand() * 0.4;
                    if (t >= SETTLE && i == 0) { if (m < s.mmin) s.mmin = m; if (m > s.mmax) s.mmax = m; if (s.nm < 1024) s.ms[s.nm++] = m; }
                }
            } else if (t >= x->next_ctl) {
                x->duty = old_step(x->duty, r, cap, dmax);
                x->next_ctl = t + 2.0;
                if (t >= SETTLE && i == 0) { if (r < s.mmin) s.mmin = r; if (r > s.mmax) s.mmax = r; if (s.nm < 1024) s.ms[s.nm++] = r; }
            }
        }
        (void)mwin;
        if (t < SETTLE) continue;
        if (sink < s.smin) s.smin = sink;
        if (sink > s.smax) s.smax = sink;
        s.ssum += sink; s.busy += active; s.zero += N[0].duty < 0.5; s.n++;
    }
    return s;
}

static double p95(stats *s) { median(s->ms, s->nm); return s->ms[(int)(s->nm * 0.95)]; }
static double mean_in(stats *s) { double a = 0; for (int i = 0; i < s->nm; i++) a += s->ms[i]; return a / s->nm; }

static void show(const char *name, stats s) {
    printf("  %-22s heatsink %4.1f..%4.1f C  sensor mean %4.1f p95 %4.1f max %3.0f C  work %.2f cores  paused %4.1f%%\n",
           name, s.smin, s.smax, mean_in(&s), p95(&s), s.mmax, s.busy / s.n, 100.0 * s.zero / s.n);
}

/* A brief foreign spike must not stop the node outright. Measured on this
 * laptop: at every sample the die passed 90 C, constella held 0.216 cores while
 * the rest of the machine held 1.558 -- the heat was someone else's, and
 * SCHED_IDLE already yields to it. Sustained heat must still stop. */
static int spike_test(void) {
    ctl_cfg k = mkcfg(88, 100, 50);          /* i7-8850H: crit 100 -> cap 88, hard 95 */
    ctl_t c;
    memset(&c, 0, sizeof c);
    int fails = 0;
    for (int i = 0; i < 200; i++) ctl_step(&c, &k, k.target, 1.1);
    double settled = c.duty;
    double d = ctl_step(&c, &k, 97, 1.1);
    printf("  settled %.0f%% duty at %.0f C; one 97 C window -> %.0f%%\n", settled, k.target, d);
    if (d <= 0) { printf("  FAIL: a single foreign spike stopped the node\n"); fails++; }
    for (int i = 0; i < 4; i++) d = ctl_step(&c, &k, 97, 1.1);
    printf("  five consecutive 97 C windows -> %.0f%%\n", d);
    if (d > 0) { printf("  FAIL: sustained heat did not stop the node\n"); fails++; }

    /* Riding out spikes must not extend to the chip's own limit: at TjMax the
     * CPU throttles itself and the user's work suffers, so stop immediately. */
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 200; i++) ctl_step(&c, &k, k.target, 1.1);
    d = ctl_step(&c, &k, 99, 1.1);
    printf("  one 99 C window (1 C under critical) -> %.0f%%\n", d);
    if (d > 0) { printf("  FAIL: did not stop at the chip's own limit\n"); fails++; }
    return fails;
}

int main(void) {
    int fails = 0;
    printf("5 nodes x 1 thread, 50%% duty max, browser in background (after %.0f s settle):\n", SETTLE);
    stats o70 = run(5, 50, 0, 70, 1, 1);
    stats o80 = run(5, 50, 0, 80, 1, 1);
    show("old, cap 70", o70);
    show("old, cap 80", o80);
    for (unsigned seed = 1; seed <= 5; seed++) {
        stats n = run(5, 50, 1, 80, 1, seed);
        char name[40];
        snprintf(name, sizeof name, "new, cap 80 (seed %u)", seed);
        show(name, n);
        if (n.smax - n.smin > 5)    { printf("  FAIL: heatsink swings %.1f C\n", n.smax - n.smin); fails++; }
        if (p95(&n) > 80)           { printf("  FAIL: sustained temperature above cap\n"); fails++; }
        if (fabs(mean_in(&n) - 74) > 3) { printf("  FAIL: not settled near target\n"); fails++; }
        if (n.mmax >= 90)           { printf("  FAIL: reached hard stop\n"); fails++; }
        if (n.zero / n.n > 0.02)    { printf("  FAIL: paused %.1f%% of the time\n", 100.0 * n.zero / n.n); fails++; }
        if (n.busy / n.n < o70.busy / o70.n * 1.5) { puts("  FAIL: not more useful work than before"); fails++; }
    }
    printf("\n1 node x 1 thread, quiet machine (must not throttle):\n");
    stats one = run(1, 50, 1, 80, 0, 7);
    show("new, cap 80", one);
    if (one.busy / one.n < 0.45) { puts("  FAIL: throttled a cool machine"); fails++; }

    printf("\nforeign heat spikes (cap 88, hard 95):\n");
    fails += spike_test();

    printf("\n%s\n", fails ? "thermal sim: FAIL" : "thermal sim: ok");
    return fails != 0;
}
