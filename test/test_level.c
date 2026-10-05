/* Host-Tests: zig cc -I../firmware/src test_level.c ../firmware/src/level.c */
#include <stdio.h>
#include <string.h>
#include "level.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const level_cfg_t cfg = { 3000, 250, 3200, 30, 5, 10 };

/* us fuer Distanz mm bei 10 C (c = 337.36 m/s) */
static uint16_t us_for(uint32_t mm) { return (uint16_t)((mm * 200000u + 16869u) / 33736u); }

int main(void)
{
    /* Umrechnung */
    CHECK(echo_us_to_mm(5824, 20) == 1000);          /* 343.4 m/s */
    CHECK(echo_us_to_mm(us_for(2000), 10) == 2000);
    CHECK(echo_us_to_mm(0, 10) == 0);
    CHECK(echo_us_to_mm(us_for(1000), -20) < 1000);  /* kalt -> langsamer */

    /* Normalfall: Wasser 1,2 m unter Sensor -> 1800 mm Pegel */
    uint16_t s[9];
    for (int i = 0; i < 9; i++) s[i] = us_for(1200 + (i % 3) - 1);
    level_meas_t m = level_evaluate(&cfg, s, 9);
    CHECK(m.status == ST_OK && m.dist_mm == 1200 && m.level_mm == 1800 && m.valid == 9);

    /* 3 Ausreisser (Wandecho, Timeout, Blindzone) werden verworfen */
    s[0] = us_for(600); s[3] = 0; s[5] = us_for(100);
    m = level_evaluate(&cfg, s, 9);
    CHECK(m.status == ST_OK && m.dist_mm == 1200 && m.valid == 6);

    /* Kein Echo */
    memset(s, 0, sizeof s);
    m = level_evaluate(&cfg, s, 9);
    CHECK(m.status == ST_NO_ECHO && m.valid == 0);

    /* Starke Streuung */
    for (int i = 0; i < 9; i++) s[i] = us_for(500 + 250u * i);
    m = level_evaluate(&cfg, s, 9);
    CHECK(m.status == ST_UNSTABLE);

    /* Schacht leer / Distanz minimal ueber Montagehoehe -> Pegel 0 */
    for (int i = 0; i < 9; i++) s[i] = us_for(3050);
    m = level_evaluate(&cfg, s, 9);
    CHECK(m.status == ST_OK && m.level_mm == 0);

    /* n > MAX_SAMPLES darf nicht ueberlaufen */
    uint16_t big[40];
    for (int i = 0; i < 40; i++) big[i] = us_for(1000);
    m = level_evaluate(&cfg, big, 40);
    CHECK(m.status == ST_OK && m.valid == 16);

    /* Report-Logik */
    report_state_t rs = { 0 };
    level_meas_t ok = { 1200, 1800, ST_OK, 9 }, err = { 0, 0, ST_NO_ECHO, 0 };
    CHECK(report_should_send(&rs, &ok, 0, 10, 300000, 3) == 1);       /* erstes Mal */
    ok.level_mm = 1805;
    CHECK(report_should_send(&rs, &ok, 10000, 10, 300000, 3) == 0);   /* < 1 cm */
    ok.level_mm = 1810;
    CHECK(report_should_send(&rs, &ok, 20000, 10, 300000, 3) == 1);   /* = 1 cm */
    CHECK(report_should_send(&rs, &ok, 320000, 10, 300000, 3) == 1);  /* Heartbeat */
    CHECK(report_should_send(&rs, &err, 330000, 10, 300000, 3) == 0); /* 1. Fehler */
    CHECK(report_should_send(&rs, &err, 340000, 10, 300000, 3) == 0); /* 2. Fehler */
    CHECK(report_should_send(&rs, &err, 350000, 10, 300000, 3) == 1); /* 3. -> melden */
    CHECK(report_should_send(&rs, &err, 360000, 10, 300000, 3) == 0); /* nicht spammen */
    CHECK(report_should_send(&rs, &ok, 370000, 10, 300000, 3) == 1);  /* wieder OK */

    /* ms-Zaehler-Ueberlauf (nach 49 Tagen) */
    report_state_t rw = { 1, ST_OK, 0, 1810, 0xFFFFF000u };
    CHECK(report_should_send(&rw, &ok, 0x00001000u, 10, 300000, 3) == 0);
    CHECK(report_should_send(&rw, &ok, 0x00001000u + 300000u, 10, 300000, 3) == 1);

    /* Format */
    char buf[32];
    level_meas_t f = { 65535, 65535, 2, 0 };
    int n = level_format(buf, &f);
    CHECK(strcmp(buf, "L=65535;D=65535;S=2\n") == 0 && n == 20);
    f = (level_meas_t){ 0, 0, 0, 0 };
    level_format(buf, &f);
    CHECK(strcmp(buf, "L=0;D=0;S=0\n") == 0);

    printf(fails ? "%d FEHLER\n" : "Alle Tests OK\n", fails);
    return fails != 0;
}
