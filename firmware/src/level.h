/* Hardware-unabhaengige Messlogik (auf dem PC testbar). */
#ifndef LEVEL_H
#define LEVEL_H

#include <stdint.h>

enum {
    ST_OK        = 0, /* gueltige Messung                          */
    ST_NO_ECHO   = 1, /* zu wenige gueltige Echos (Kabel, Sensor)   */
    ST_UNSTABLE  = 2, /* Echos streuen zu stark (Wandreflexionen)   */
};

typedef struct {
    uint16_t mount_mm;      /* Sensorflaeche ueber Schachtboden        */
    uint16_t min_mm;        /* Blindzone JSN-SR04T (~250 mm)           */
    uint16_t max_mm;        /* groesste plausible Distanz              */
    uint16_t window_mm;     /* Toleranz um den Median                  */
    uint8_t  min_valid;     /* min. Echos im Toleranzfenster           */
    int8_t   temp_c;        /* Lufttemperatur im Schacht               */
} level_cfg_t;

typedef struct {
    uint16_t dist_mm;       /* Sensor -> Wasseroberflaeche             */
    uint16_t level_mm;      /* Wasserhoehe ueber Schachtboden          */
    uint8_t  status;
    uint8_t  valid;         /* Anzahl verwendeter Echos                */
} level_meas_t;

typedef struct {
    uint8_t  has_sent;
    uint8_t  last_status;
    uint8_t  fail_count;
    uint16_t last_level;
    uint32_t last_ms;
} report_state_t;

uint16_t     echo_us_to_mm(uint32_t us, int8_t temp_c);
level_meas_t level_evaluate(const level_cfg_t *cfg, const uint16_t *echo_us, uint8_t n);

/* Entscheidet, ob gesendet wird. Fehler werden erst nach `fail_limit`
 * Fehlmessungen in Folge gemeldet (vermeidet Flattern). */
int  report_should_send(report_state_t *rs, level_meas_t *m, uint32_t now_ms,
                        uint16_t delta_mm, uint32_t heartbeat_ms, uint8_t fail_limit);

/* Schreibt "L=<mm>;D=<mm>;S=<status>;T=<C>\n", liefert Laenge. buf >= 32 Byte. */
int  level_format(char *buf, const level_meas_t *m, int8_t temp_c);

/* DS18B20: prueft CRC + Plausibilitaet des 9-Byte-Scratchpads.
 * Liefert 1 und *temp_c (gerundet), sonst 0. */
uint8_t ds18b20_crc8(const uint8_t *p, uint8_t n);
int     ds18b20_decode(const uint8_t sp[9], int8_t *temp_c);

#endif
