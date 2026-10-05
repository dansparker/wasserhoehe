#include "level.h"

#define MAX_SAMPLES 16

uint16_t echo_us_to_mm(uint32_t us, int8_t temp_c)
{
    /* c = 331.3 + 0.606*T m/s, in 0.01 m/s; dist = t*c/2 (passt in 32 Bit) */
    if (us > 65535u) us = 65535u;
    uint32_t c_x100 = (uint32_t)(33130 + (606 * (int32_t)temp_c) / 10);
    uint32_t mm = (us * c_x100 + 100000u) / 200000u;
    return mm > 0xFFFFu ? 0xFFFFu : (uint16_t)mm;
}

static void sort_u16(uint16_t *a, uint8_t n)
{
    for (uint8_t i = 1; i < n; i++) {
        uint16_t v = a[i];
        uint8_t j = i;
        while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

level_meas_t level_evaluate(const level_cfg_t *cfg, const uint16_t *echo_us, uint8_t n)
{
    level_meas_t m = { 0, 0, ST_NO_ECHO, 0 };
    uint16_t d[MAX_SAMPLES];
    uint8_t k = 0;

    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    for (uint8_t i = 0; i < n; i++) {
        if (echo_us[i] == 0) continue;               /* Timeout */
        uint16_t mm = echo_us_to_mm(echo_us[i], cfg->temp_c);
        if (mm >= cfg->min_mm && mm <= cfg->max_mm) d[k++] = mm;
    }
    if (k < cfg->min_valid) { m.valid = k; return m; }

    sort_u16(d, k);
    uint16_t med = d[k / 2];

    /* Nur Echos nahe dem Median mitteln -> Ausreisser fliegen raus */
    uint32_t sum = 0;
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < k; i++) {
        uint16_t diff = d[i] > med ? d[i] - med : med - d[i];
        if (diff <= cfg->window_mm) { sum += d[i]; cnt++; }
    }
    m.valid = cnt;
    if (cnt < cfg->min_valid) { m.status = ST_UNSTABLE; return m; }

    m.dist_mm  = (uint16_t)((sum + cnt / 2) / cnt);
    m.level_mm = m.dist_mm >= cfg->mount_mm ? 0 : (uint16_t)(cfg->mount_mm - m.dist_mm);
    m.status   = ST_OK;
    return m;
}

int report_should_send(report_state_t *rs, level_meas_t *m, uint32_t now_ms,
                       uint16_t delta_mm, uint32_t heartbeat_ms, uint8_t fail_limit)
{
    if (m->status != ST_OK) {
        if (rs->fail_count < 255) rs->fail_count++;
        if (rs->fail_count < fail_limit) return 0;   /* Einzelfehler ignorieren */
    } else {
        rs->fail_count = 0;
    }

    int send = !rs->has_sent
            || m->status != rs->last_status
            || (uint32_t)(now_ms - rs->last_ms) >= heartbeat_ms;

    if (!send && m->status == ST_OK) {
        uint16_t diff = m->level_mm > rs->last_level ? m->level_mm - rs->last_level
                                                     : rs->last_level - m->level_mm;
        send = diff >= delta_mm;
    }
    if (send) {
        rs->has_sent    = 1;
        rs->last_status = m->status;
        rs->last_ms     = now_ms;
        if (m->status == ST_OK) rs->last_level = m->level_mm;
    }
    return send;
}

static char *put_u(char *p, uint32_t v)
{
    char tmp[10];
    int i = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) *p++ = tmp[--i];
    return p;
}

int level_format(char *buf, const level_meas_t *m, int8_t temp_c)
{
    char *p = buf;
    *p++ = 'L'; *p++ = '='; p = put_u(p, m->level_mm);
    *p++ = ';'; *p++ = 'D'; *p++ = '='; p = put_u(p, m->dist_mm);
    *p++ = ';'; *p++ = 'S'; *p++ = '='; p = put_u(p, m->status);
    *p++ = ';'; *p++ = 'T'; *p++ = '=';
    if (temp_c < 0) { *p++ = '-'; p = put_u(p, (uint32_t)(-(int32_t)temp_c)); }
    else p = put_u(p, (uint32_t)temp_c);
    *p++ = '\n';
    *p = '\0';
    return (int)(p - buf);
}

uint8_t ds18b20_crc8(const uint8_t *p, uint8_t n)
{
    uint8_t crc = 0;                                 /* Dallas/Maxim, Poly 0x8C */
    while (n--) {
        uint8_t b = *p++;
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1u;
            crc >>= 1;
            if (mix) crc ^= 0x8Cu;
            b >>= 1;
        }
    }
    return crc;
}

int ds18b20_decode(const uint8_t sp[9], int8_t *temp_c)
{
    int all_same = 1;                                /* Bus offen (FF) oder kurz (00) */
    for (uint8_t i = 1; i < 9; i++) if (sp[i] != sp[0]) all_same = 0;
    if (all_same) return 0;
    if (ds18b20_crc8(sp, 8) != sp[8]) return 0;

    int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
    if (raw == 0x0550) return 0;                     /* 85 C = Einschaltwert, keine Messung */
    int32_t t = raw >= 0 ? (raw + 8) / 16 : -((-raw + 8) / 16);
    if (t < -40 || t > 60) return 0;                 /* fuer einen Schacht unplausibel */
    *temp_c = (int8_t)t;
    return 1;
}
