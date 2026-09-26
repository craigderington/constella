#include "addr.h"
#include <string.h>

static int v4mapped(const uint8_t ip[16]) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    return !memcmp(ip, pfx, 12);
}

int addr_netgroup(const uint8_t ip[16], uint8_t out[8]) {
    if (v4mapped(ip)) { out[0] = ip[12]; out[1] = ip[13]; return 2; }
    memcpy(out, ip, 4);
    return 4;
}

int addr_is_routable(const uint8_t ip[16]) {
    if (v4mapped(ip)) {
        uint8_t a = ip[12], b = ip[13];
        if (a == 0 || a == 127 || a == 10) return 0;
        if (a == 192 && b == 168) return 0;
        if (a == 172 && (b & 0xf0) == 16) return 0;
        if (a == 169 && b == 254) return 0;
        if (a >= 224) return 0;                       /* multicast, reserved */
        return 1;
    }
    static const uint8_t zero[16] = {0};
    if (!memcmp(ip, zero, 16)) return 0;              /* :: */
    if (!memcmp(ip, zero, 15) && ip[15] == 1) return 0; /* ::1 */
    if ((ip[0] & 0xfe) == 0xfc) return 0;             /* fc00::/7 ULA */
    if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) return 0; /* fe80::/10 */
    if (ip[0] == 0xff) return 0;                      /* multicast */
    return 1;
}
