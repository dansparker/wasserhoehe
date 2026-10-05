/*
 * Wasserhoehe im Brunnenschacht - STM32F103C8T6 (Bluepill)
 *
 *  JSN-SR04T  TRIG -> PB7        ECHO (5 V) -> PB6 (5V-tolerant)
 *  CC2530     RX (P0.2) <- PA9 (USART1 TX, 9600 8N1)
 *  DS18B20    DQ -> PB8 (optional, 4,7 kOhm Pull-up nach 3,3 V)
 *  LED        PC13 (aktiv low), blinkt bei jeder Messung
 */
#include "stm32f1xx.h"
#include "level.h"

/* ---------- Konfiguration ------------------------------------------ */
#define MOUNT_MM        3000u    /* Sensorflaeche ueber Schachtboden  */
#define TEMP_DEFAULT_C  10       /* ohne DS18B20 / bei Lesefehler     */
#define TEMP_KEEP       6        /* letzten DS18B20-Wert so oft halten */
#define SAMPLES         9        /* Pings pro Messung                 */
#define PING_GAP_MS     70       /* Nachhall abklingen lassen         */
#define MEAS_PERIOD_MS  10000u   /* alle 10 s messen                  */
#define DELTA_MM        10u      /* senden bei >= 1 cm Aenderung      */
#define HEARTBEAT_MS    300000u  /* spaetestens alle 5 min senden     */
#define FAIL_LIMIT      3        /* Fehler erst nach 3 Fehlmessungen  */
#define UART_BAUD       9600u
#define ECHO_TIMEOUT_US 30000u   /* ~5 m                              */

#define TRIG_PIN  (1u << 7)
#define ECHO_PIN  (1u << 6)
#define LED_PIN   (1u << 13)
#define OW_PIN    (1u << 8)
#define ECHO_HIGH (GPIOB->IDR & ECHO_PIN)

static volatile uint32_t ms_ticks;
static uint32_t sysclk_hz, pclk1_tim_hz, pclk2_hz;

void SysTick_Handler(void) { ms_ticks++; }

static void clock_init(void)
{
    /* HSE 8 MHz -> PLL x9 = 72 MHz; Fallback HSI 8 MHz (Clone ohne Quarz) */
    RCC->CR |= RCC_CR_HSEON;
    for (uint32_t t = 0; t < 200000u && !(RCC->CR & RCC_CR_HSERDY); t++) { }

    if (RCC->CR & RCC_CR_HSERDY) {
        FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
        RCC->CFGR  = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
        RCC->CR   |= RCC_CR_PLLON;
        while (!(RCC->CR & RCC_CR_PLLRDY)) { }
        RCC->CFGR |= RCC_CFGR_SW_PLL;
        while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }
        sysclk_hz = 72000000u; pclk1_tim_hz = 72000000u; pclk2_hz = 72000000u;
    } else {
        RCC->CR &= ~RCC_CR_HSEON;
        sysclk_hz = pclk1_tim_hz = pclk2_hz = 8000000u;
    }
}

static void periph_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN
                  | RCC_APB2ENR_USART1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    /* PB7 Push-Pull 2 MHz, PB6 Eingang mit Pull-down (offenes Kabel = low) */
    GPIOB->CRL = (GPIOB->CRL & ~(0xFFu << 24)) | (0x2u << 28) | (0x8u << 24);
    GPIOB->BRR = TRIG_PIN | ECHO_PIN;
    /* PB8 Open-Drain 2 MHz, losgelassen (1-Wire) */
    GPIOB->CRH = (GPIOB->CRH & ~0xFu) | 0x6u;
    GPIOB->BSRR = OW_PIN;
    /* PA9 AF Push-Pull 2 MHz */
    GPIOA->CRH = (GPIOA->CRH & ~(0xFu << 4)) | (0xAu << 4);
    /* PC13 Push-Pull 2 MHz, LED aus */
    GPIOC->CRH = (GPIOC->CRH & ~(0xFu << 20)) | (0x2u << 20);
    GPIOC->BSRR = LED_PIN;

    /* TIM2: freilaufend, 1 MHz */
    TIM2->PSC = (uint16_t)(pclk1_tim_hz / 1000000u - 1u);
    TIM2->ARR = 0xFFFF;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 = TIM_CR1_CEN;

    USART1->BRR = (uint16_t)((pclk2_hz + UART_BAUD / 2u) / UART_BAUD);
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;

    SysTick_Config(sysclk_hz / 1000u);

    /* Watchdog: LSI 40 kHz / 256 * 4095 ~ 26 s */
    IWDG->KR  = 0x5555;
    IWDG->PR  = 6;
    IWDG->RLR = 0xFFF;
    IWDG->KR  = 0xCCCC;
}

static inline void wdg_feed(void) { IWDG->KR = 0xAAAA; }

static void delay_us(uint16_t us)
{
    uint16_t t0 = (uint16_t)TIM2->CNT;
    while ((uint16_t)(TIM2->CNT - t0) < us) { }
}

static void delay_ms(uint32_t ms)
{
    uint32_t t0 = ms_ticks;
    while (ms_ticks - t0 < ms) __WFI();
}

/* Liefert Echo-Laufzeit in us, 0 bei Fehler/Timeout. */
static uint16_t ping_us(void)
{
    uint16_t t0 = (uint16_t)TIM2->CNT;
    while (ECHO_HIGH)                                  /* altes Echo noch aktiv */
        if ((uint16_t)(TIM2->CNT - t0) > ECHO_TIMEOUT_US) return 0;

    GPIOB->BSRR = TRIG_PIN;
    delay_us(20);                                      /* >= 10 us, v3 mag 20 */
    GPIOB->BRR = TRIG_PIN;

    t0 = (uint16_t)TIM2->CNT;
    while (!ECHO_HIGH)
        if ((uint16_t)(TIM2->CNT - t0) > 10000u) return 0;

    __disable_irq();                                   /* SysTick-Jitter vermeiden */
    uint16_t ts = (uint16_t)TIM2->CNT, dt;
    while (ECHO_HIGH) {
        dt = (uint16_t)(TIM2->CNT - ts);
        if (dt > ECHO_TIMEOUT_US) { __enable_irq(); return 0; }
    }
    dt = (uint16_t)(TIM2->CNT - ts);
    __enable_irq();
    return dt;
}

/* ---------- 1-Wire / DS18B20 ------------------------------------------ */
#define OW_LOW()   (GPIOB->BRR  = OW_PIN)
#define OW_REL()   (GPIOB->BSRR = OW_PIN)
#define OW_READ()  ((GPIOB->IDR & OW_PIN) != 0)

static int ow_reset(void)
{
    if (!OW_READ()) return 0;                          /* Bus kurz / kein Pull-up */
    OW_LOW();  delay_us(480);
    __disable_irq();
    OW_REL();  delay_us(70);
    int present = !OW_READ();
    __enable_irq();
    delay_us(410);
    return present;
}

static int ow_bit(int b)
{
    __disable_irq();
    OW_LOW();  delay_us(b ? 3 : 60);
    OW_REL();
    if (b) delay_us(9);
    int r = OW_READ();
    __enable_irq();
    delay_us(b ? 55 : 10);
    return r;
}

static void ow_write(uint8_t v)
{
    for (int i = 0; i < 8; i++, v >>= 1) ow_bit(v & 1u);
}

static uint8_t ow_read(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint8_t)(ow_bit(1) << i);
    return v;
}

/* Startet Wandlung (750 ms, laeuft parallel zu den Pings). */
static int ds_start(void)
{
    if (!ow_reset()) return 0;
    ow_write(0xCC);                                    /* Skip ROM: nur ein Sensor */
    ow_write(0x44);                                    /* Convert T */
    return 1;
}

static int ds_read(int8_t *t)
{
    uint8_t sp[9];
    uint32_t t0 = ms_ticks;
    while (!ow_bit(1))                                 /* 1 = Wandlung fertig */
        if (ms_ticks - t0 > 800u) return 0;
    if (!ow_reset()) return 0;
    ow_write(0xCC);
    ow_write(0xBE);                                    /* Read Scratchpad */
    for (int i = 0; i < 9; i++) sp[i] = ow_read();
    return ds18b20_decode(sp, t);
}

static void uart_puts(const char *s)
{
    while (*s) {
        while (!(USART1->SR & USART_SR_TXE)) { }
        USART1->DR = (uint8_t)*s++;
    }
    while (!(USART1->SR & USART_SR_TC)) { }
}

int main(void)
{
    static level_cfg_t cfg = {
        .mount_mm  = MOUNT_MM,
        .min_mm    = 250,
        .max_mm    = MOUNT_MM + 200,   /* Toleranz fuer Temperaturfehler */
        .window_mm = 30,
        .min_valid = SAMPLES / 2 + 1,
        .temp_c    = TEMP_DEFAULT_C,
    };
    report_state_t rs = { 0 };
    uint16_t echo[SAMPLES];
    char msg[32];
    uint8_t temp_fail = TEMP_KEEP;

    clock_init();
    periph_init();
    delay_ms(3000);                    /* CC2530 booten / Netz beitreten lassen */

    uint32_t last = ms_ticks - MEAS_PERIOD_MS;
    for (;;) {
        wdg_feed();
        if (ms_ticks - last >= MEAS_PERIOD_MS) {
            last = ms_ticks;
            GPIOC->BRR = LED_PIN;
            int ds = ds_start();
            for (int i = 0; i < SAMPLES; i++) {
                echo[i] = ping_us();
                delay_ms(PING_GAP_MS);
            }
            GPIOC->BSRR = LED_PIN;

            int8_t t;
            if (ds && ds_read(&t)) {
                cfg.temp_c = t;
                temp_fail = 0;
            } else if (temp_fail < TEMP_KEEP) {
                temp_fail++;                           /* Einzelfehler: alten Wert halten */
            } else {
                cfg.temp_c = TEMP_DEFAULT_C;           /* kein Sensor angeschlossen */
            }

            level_meas_t m = level_evaluate(&cfg, echo, SAMPLES);
            if (report_should_send(&rs, &m, ms_ticks, DELTA_MM, HEARTBEAT_MS, FAIL_LIMIT)) {
                level_format(msg, &m, cfg.temp_c);
                uart_puts(msg);
            }
        }
        __WFI();
    }
}
