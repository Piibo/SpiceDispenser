#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

// =================== Pins ===================
#define STEP_PIN   GPIO_NUM_4
#define DIR_PIN    GPIO_NUM_5
#define EN_PIN     GPIO_NUM_6        // A4988: Enable aktiv LOW

#define BTN_STEP   GPIO_NUM_7        // Taster 1 -> Stepper (nach GND)
#define BTN_SERVO  GPIO_NUM_8        // Taster 2 -> Servo (nach GND)

#define SERVO_PIN  GPIO_NUM_9        // Servo-Signal (PWM)
// ============================================

// ========== Stepper-Parameter ==========
#define STEPS_PER_REV  200           // Vollschritt: 200 Schritte = 1 U
#define PULSE_US       800           // Pulsdauer High/Low in µs (~625 SPS)
// ======================================

// ========== Servo-Parameter ============
#define SERVO_FREQ_HZ       50
#define SERVO_TIMER_MODE    LEDC_LOW_SPEED_MODE
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_DUTY_RES      LEDC_TIMER_13_BIT

// Endpunkte (anpassen!)
#define SERVO_START_US      910      // ca. ~0..10°
#define SERVO_END_US        1090     // ca. ~170..180°
#define SERVO_STEP_US       8        // Schrittweite für sanfte Bewegung
#define SERVO_STEP_DELAY_MS 6        // Pause zwischen Schritten
// ======================================

static const char *TAG = "BTN_STEPPER_SERVO";

// ---------------- Stepper helpers ----------------
static inline void step_pulse_us(int pulse_us) {
    gpio_set_level(STEP_PIN, 1);
    esp_rom_delay_us(pulse_us);
    gpio_set_level(STEP_PIN, 0);
    esp_rom_delay_us(pulse_us);
}

static void move_steps_dir(int steps, int pulse_us, int dir_level) {
    gpio_set_level(DIR_PIN, dir_level);
    esp_rom_delay_us(5); // kleine DIR-Setup-Zeit
    for (int i = 0; i < steps; i++) step_pulse_us(pulse_us);
}

// ---------------- Button debounce ----------------
typedef struct {
    int last_level;
    int stable_level;
    int stable_count;
} deb_state_t;

static int debounce_read(gpio_num_t pin, deb_state_t *st) {
    int level = gpio_get_level(pin);
    if (level == st->last_level) {
        if (st->stable_count < 5) st->stable_count++;
    } else {
        st->stable_count = 0;
        st->last_level = level;
    }
    if (st->stable_count >= 5) st->stable_level = level;
    return st->stable_level;
}

static bool edge_falling(gpio_num_t pin, deb_state_t *st) {
    static int prev[32] = {0};
    int val = debounce_read(pin, st);
    bool falling = (prev[pin] == 1 && val == 0); // Pull-up: 1->0 beim Drücken
    prev[pin] = val;
    return falling;
}

// ---------------- Servo helpers ----------------
static inline uint32_t servo_us_to_duty(uint32_t us) {
    const uint32_t period_us = 1000000UL / SERVO_FREQ_HZ;        // 20000
    const uint32_t max_duty  = (1U << SERVO_DUTY_RES) - 1U;      // 8191 bei 13 Bit
    if (us < 500)  us = 500;                                     // Sicherheits-Clamps
    if (us > 2500) us = 2500;
    return (uint32_t)((uint64_t)us * max_duty / period_us);
}

static void servo_write_us(uint32_t us) {
    uint32_t duty = servo_us_to_duty(us);
    ledc_set_duty(SERVO_TIMER_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(SERVO_TIMER_MODE, SERVO_CHANNEL);
}

static void servo_move_smooth(uint32_t from_us, uint32_t to_us) {
    if (from_us == to_us) { servo_write_us(from_us); return; }
    int step = (to_us > from_us) ? SERVO_STEP_US : -(int)SERVO_STEP_US;
    int pos = (int)from_us;
    while ((step > 0 && pos < (int)to_us) || (step < 0 && pos > (int)to_us)) {
        pos += step;
        servo_write_us((uint32_t)pos);
        vTaskDelay(pdMS_TO_TICKS(SERVO_STEP_DELAY_MS));
    }
    servo_write_us(to_us);
}

void app_main(void) {
    // --- Stepper-IOs ---
    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL<<STEP_PIN) | (1ULL<<DIR_PIN) | (1ULL<<EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_out);

    // Enable Treiber (aktiv LOW)
    gpio_set_level(EN_PIN, 0);
    esp_rom_delay_us(5000);

    // --- Buttons (Pull-Up, Taster nach GND) ---
    gpio_config_t io_btn = {
        .pin_bit_mask = (1ULL<<BTN_STEP) | (1ULL<<BTN_SERVO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_btn);
    deb_state_t db_step = {.last_level=1, .stable_level=1, .stable_count=0};
    deb_state_t db_ser  = {.last_level=1, .stable_level=1, .stable_count=0};

    // --- Servo PWM (LEDC) ---
    ledc_timer_config_t tcfg = {
        .speed_mode       = SERVO_TIMER_MODE,
        .duty_resolution  = SERVO_DUTY_RES,
        .timer_num        = SERVO_TIMER,
        .freq_hz          = SERVO_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = SERVO_PIN,
        .speed_mode = SERVO_TIMER_MODE,
        .channel    = SERVO_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SERVO_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ccfg);

    // Servo auf Startposition initialisieren
    uint32_t servo_pos_us = SERVO_START_US;
    servo_write_us(servo_pos_us);

    ESP_LOGI(TAG, "Ready. BTN_STEP=GPIO%d, BTN_SERVO=GPIO%d, SERVO=GPIO%d",
             BTN_STEP, BTN_SERVO, SERVO_PIN);

    while (1) {
        // Button 1 gedrückt? -> Stepper vor/zurück (1 U)
        if (edge_falling(BTN_STEP, &db_step)) {
            ESP_LOGI(TAG, "Stepper: Vor 1U, Pause, Zurueck 1U");
            move_steps_dir(STEPS_PER_REV, PULSE_US, 1);  // vorwärts
            vTaskDelay(pdMS_TO_TICKS(300));
            move_steps_dir(STEPS_PER_REV, PULSE_US, 0);  // rückwärts
        }

        // Button 2 gedrückt? -> Servo Start <-> End sanft toggeln
        if (edge_falling(BTN_SERVO, &db_ser)) {
            uint32_t next = (servo_pos_us == SERVO_START_US) ? SERVO_END_US : SERVO_START_US;
            ESP_LOGI(TAG, "Servo: %u us -> %u us", (unsigned)servo_pos_us, (unsigned)next);
            servo_move_smooth(servo_pos_us, next);
            servo_pos_us = next;
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Polling + Entprellung
    }
}
