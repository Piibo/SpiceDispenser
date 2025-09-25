#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

// =================== Pins ===================
#define STEP_PIN    GPIO_NUM_4
#define DIR_PIN     GPIO_NUM_5
#define EN_PIN      GPIO_NUM_6        // A4988: Enable aktiv LOW
#define BTN_STEP    GPIO_NUM_7        // Taster -> Start Zyklus
#define BTN_SERVO   GPIO_NUM_8        // Taster -> Servo manuell toggeln (optional)
#define SERVO_PIN   GPIO_NUM_9        // Servo-Signal (PWM)

// =================== Konfig ===================
#define STEP_PULSE_HIGH_US          4

// -- Altwerte (nur noch Referenz) --
// #define STEP_INTERSTEP_US_MOVE      1800
// #define STEP_INTERSTEP_US_DISPENSE  2400

// === RAMPENPROFILE ===
// Kleinere Mikrosekunden = schneller
typedef struct {
    int us_min;            // schnellste Interstep-Zeit (Plateau)
    int us_max;            // langsamste Interstep-Zeit (Start/Ende)
    int accel_per_step_us; // Änderung der Interstep-Zeit je Schritt (linear)
} speed_profile_t;

// Fahrt (Wagen bewegen)
#define MOVE_US_MIN            2500     // zügig
#define MOVE_US_MAX            4000    // sanfter Start/Stopp
#define MOVE_ACCEL_PER_STEP     25     // je Schritt ~25 us schneller/langsamer

// Ausgabe (Spindel drehen)
#define DISP_US_MIN           1200
#define DISP_US_MAX           2600
#define DISP_ACCEL_PER_STEP     20

static const speed_profile_t PROFILE_MOVE = {
    .us_min = MOVE_US_MIN,
    .us_max = MOVE_US_MAX,
    .accel_per_step_us = MOVE_ACCEL_PER_STEP
};
static const speed_profile_t PROFILE_DISP = {
    .us_min = DISP_US_MIN,
    .us_max = DISP_US_MAX,
    .accel_per_step_us = DISP_ACCEL_PER_STEP
};

#define SERVO_FREQ_HZ       50
#define SERVO_TIMER_MODE    LEDC_LOW_SPEED_MODE
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_DUTY_RES      LEDC_TIMER_13_BIT
#define SERVO_BACK_US       910      // hinten  = Auslass-Kupplung
#define SERVO_FRONT_US      1090     // vorne  = Fahr-Kupplung
#define SERVO_STEP_US       8
#define SERVO_STEP_DELAY_MS 12
#define SERVO_SETTLE_MS     200

#define PAUSE_BEFORE_DECOUPLE 250    // ms: Pause VOR jedem Auskoppeln (Fahrt -> Ausgabe)
#define PAUSE_AFTER_COUPLE    150
#define PAUSE_BEFORE_MOVE     120
#define PAUSE_AFTER_DISP      80

// === Mechanik ===
#define TRAVEL_STEPS_PER_STOP   88     // Schritte zwischen zwei Wagen-Positionen
#define DISPENSE_STEPS_PER_REV  200    // Schritte für 1 volle Spindel-Umdrehung (anpassen)

// === Wagenanzahl (konfigurierbar) ===
#define POS_COUNT_DEFAULT       4      // <-- HIER die Anzahl deiner Wagen einstellen
#define MAX_POS                 32

// === Gewürz-Strings ===
#define SPICE_NAME_MAX          24

static const char *TAG = "SPICE_RUN";

// =================== Zustände ===================
static long long abs_steps = 0;   // nur fürs Logging
static int current_index = 0;     // 0..pos_count-1
static int pos_count = POS_COUNT_DEFAULT;

// Regal-Mapping (pro Wagen ein Name, lowercased gespeichert)
static char spices_map[MAX_POS][SPICE_NAME_MAX];

// =================== Stepper low-level ===================
static inline void step_pulse_once(void){
    gpio_set_level(STEP_PIN, 1);
    esp_rom_delay_us(STEP_PULSE_HIGH_US);
    gpio_set_level(STEP_PIN, 0);
}
static inline void set_dir(int lvl){
    gpio_set_level(DIR_PIN, lvl);
    esp_rom_delay_us(5);
}

// ---- NEU: Bewegung mit Beschl./Bremsrampe (Trapezprofil) ----
static void move_steps_ramped(int delta_steps, const speed_profile_t *p){
    if(delta_steps == 0 || !p) return;

    // Korrigierte Grenzen
    int us_min = (p->us_min < 200) ? 200 : p->us_min;   // nicht zu schnell
    int us_max = (p->us_max < us_min) ? us_min : p->us_max;
    int d_us   = (p->accel_per_step_us < 1) ? 1 : p->accel_per_step_us;

    int dir  = (delta_steps>0) ? 1 : 0;
    int cnt  = (delta_steps>0) ?  delta_steps : -delta_steps;

    set_dir(dir);

    // Schritte zum Beschleunigen/Bremsen (linear)
    int accel_steps = (us_max - us_min + d_us - 1) / d_us; // aufrunden
    int decel_steps = accel_steps;

    // Triangular-Fall abfangen (zu kurze Bewegungen)
    if (2*accel_steps > cnt) {
        accel_steps = cnt / 2;
        decel_steps = cnt - accel_steps;
    }
    int plateau_steps = cnt - accel_steps - decel_steps;

    int us = us_max;

    // Beschleunigen
    for (int i=0; i<accel_steps; ++i){
        step_pulse_once();
        esp_rom_delay_us(us);
        abs_steps += (dir? +1 : -1);
        us -= d_us;
        if (us < us_min) us = us_min;
    }

    // Plateau
    for (int i=0; i<plateau_steps; ++i){
        step_pulse_once();
        esp_rom_delay_us(us);
        abs_steps += (dir? +1 : -1);
    }

    // Bremsen
    for (int i=0; i<decel_steps; ++i){
        step_pulse_once();
        esp_rom_delay_us(us);
        abs_steps += (dir? +1 : -1);
        us += d_us;
        if (us > us_max) us = us_max;
    }
}

// =================== Servo helpers ===================
static inline uint32_t servo_us_to_duty(uint32_t us){
    const uint32_t period_us = 1000000UL / SERVO_FREQ_HZ;
    const uint32_t max_duty  = (1U << SERVO_DUTY_RES) - 1U;
    if (us < 500U) us = 500U;
    else if (us > 2500U) us = 2500U;
    return (uint32_t)(((uint64_t)us * max_duty) / period_us);
}
static void servo_write_us(uint32_t us){
    uint32_t duty = servo_us_to_duty(us);
    ledc_set_duty(SERVO_TIMER_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(SERVO_TIMER_MODE, SERVO_CHANNEL);
}
static void servo_move_smooth(uint32_t from_us,uint32_t to_us){
    if(from_us==to_us){ servo_write_us(from_us); return; }
    int step = (to_us>from_us)? SERVO_STEP_US : -(int)SERVO_STEP_US;
    int pos=(int)from_us;
    while((step>0 && pos<(int)to_us) || (step<0 && pos>(int)to_us)){
        pos+=step; servo_write_us((uint32_t)pos);
        vTaskDelay(pdMS_TO_TICKS(SERVO_STEP_DELAY_MS));
    }
    servo_write_us(to_us);
}
static uint32_t servo_to_back(uint32_t cur){
    if(cur!=SERVO_BACK_US){ servo_move_smooth(cur,SERVO_BACK_US); cur=SERVO_BACK_US; }
    vTaskDelay(pdMS_TO_TICKS(SERVO_SETTLE_MS));
    return cur;
}
static uint32_t servo_to_front(uint32_t cur){
    if(cur!=SERVO_FRONT_US){ servo_move_smooth(cur,SERVO_FRONT_US); cur=SERVO_FRONT_US; }
    vTaskDelay(pdMS_TO_TICKS(SERVO_SETTLE_MS));
    return cur;
}

// --- Pause vor jedem Auskoppeln (FRONT -> BACK) ---
static uint32_t decouple_to_back_with_pause(uint32_t cur){
    if(cur != SERVO_BACK_US){
        vTaskDelay(pdMS_TO_TICKS(PAUSE_BEFORE_DECOUPLE));
    }
    return servo_to_back(cur);
}

// =================== Bewegungen (Positionslogik) ===================
static void goto_index(int target_idx){
    if(target_idx < 0) target_idx = 0;
    if(target_idx >= pos_count) target_idx = pos_count - 1;

    int delta_idx   = target_idx - current_index;
    int delta_steps = delta_idx * TRAVEL_STEPS_PER_STOP;

    ESP_LOGI(TAG,"Fahrt: Pos%d -> Pos%d  steps=%d",
             current_index+1, target_idx+1, delta_steps);

    move_steps_ramped(delta_steps, &PROFILE_MOVE);
    current_index = target_idx;
}

static void dispense_one_full_rev(void){
    ESP_LOGI(TAG,"Ausgabe @Pos%d: 1 volle Umdrehung (%d steps)",
             current_index+1, DISPENSE_STEPS_PER_REV);
    move_steps_ramped(DISPENSE_STEPS_PER_REV, &PROFILE_DISP);
}

// =================== Debounce ===================
typedef struct { int last_level; int stable_level; int stable_count; } deb_state_t;
static int debounce_read(gpio_num_t pin, deb_state_t *st){
    int level = gpio_get_level(pin);
    if(level==st->last_level){ if(st->stable_count<5) st->stable_count++; }
    else { st->stable_count=0; st->last_level=level; }
    if(st->stable_count>=5) st->stable_level=level;
    return st->stable_level;
}
static bool edge_falling(gpio_num_t pin, deb_state_t *st){
    static int prev[64]={0}; // reicht für GPIO 0..63
    int val = debounce_read(pin,st);
    bool falling = (prev[pin]==1 && val==0);
    prev[pin]=val;
    return falling;
}

// =================== Regal-Mapping & Matching ===================
static void spice_clear_all(void){
    for(int i=0;i<MAX_POS;i++) spices_map[i][0]='\0';
}
static void lowercase_inplace(char *s){
    for(size_t i=0; s && s[i]; ++i) s[i]=(char)tolower((unsigned char)s[i]);
}
static void spice_set(int idx, const char *name){
    if(idx<0 || idx>=MAX_POS || !name) return;
    strncpy(spices_map[idx], name, SPICE_NAME_MAX-1);
    spices_map[idx][SPICE_NAME_MAX-1]='\0';
    lowercase_inplace(spices_map[idx]); // normalisiert
}
static int find_pos_by_name(const char *name){
    if(!name) return -1;
    char tmp[SPICE_NAME_MAX];
    strncpy(tmp, name, SPICE_NAME_MAX-1);
    tmp[SPICE_NAME_MAX-1]='\0';
    lowercase_inplace(tmp);
    for(int i=0;i<pos_count;i++){
        if(spices_map[i][0]=='\0') continue;
        if(strcasecmp(spices_map[i], tmp)==0) return i;
    }
    return -1;
}

/**
 * Baut aus einer Gewürzliste die Ziel-Wagen (Indices) in der Reihenfolge der Liste.
 * unique_positions=true: denselben Wagen nur einmal anfahren (auch wenn Gewürz mehrfach verlangt).
 * Gibt Anzahl Targets zurück.
 */
static int build_targets_from_spices(const char *recipe_spices[], int recipe_len,
                                     int *out_indices, int out_max,
                                     bool unique_positions)
{
    if(!recipe_spices || !out_indices || out_max<=0) return 0;
    bool used[MAX_POS]={0};
    int n=0;
    ESP_LOGI(TAG,"Abgleich Gewürzliste (%d Einträge) mit Regal...", recipe_len);

    for(int i=0;i<recipe_len && n<out_max;i++){
        const char *want = recipe_spices[i];
        int pos = find_pos_by_name(want);
        if(pos<0){
            ESP_LOGW(TAG,"Nicht vorhanden: '%s'", want);
            continue;
        }
        if(unique_positions && used[pos]){
            ESP_LOGI(TAG,"Schon in Targets: '%s' @Pos%d -> übersprungen", want, pos+1);
            continue;
        }
        used[pos]=true;
        out_indices[n++] = pos;
        ESP_LOGI(TAG,"Match: '%s' -> Pos%d", want, pos+1);
    }
    ESP_LOGI(TAG,"Targets gesamt: %d", n);
    return n;
}

// =================== Zyklen ===================
/** Nur die angegebenen Ziele abfahren und ausgeben, danach zurück zu Pos1 + Park. */
static void run_cycle_targets(uint32_t *p_servo_pos, const int *targets, int n_targets){
    if(!p_servo_pos || !targets || n_targets<=0) return;

    // Sicherheitsgrenzen
    if(pos_count < 1) pos_count = 1;
    if(pos_count > MAX_POS) pos_count = MAX_POS;

    // Auf "Fahren" kuppeln
    *p_servo_pos = servo_to_front(*p_servo_pos);
    vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));

    // Ziele in angegebener Reihenfolge anfahren
    for(int j=0;j<n_targets;j++){
        int idx = targets[j];
        if(idx<0 || idx>=pos_count) continue;

        goto_index(idx);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_BEFORE_MOVE));

        // Pause vor Auskoppeln + Ausgabe
        *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));

        dispense_one_full_rev();
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_DISP));

        *p_servo_pos = servo_to_front(*p_servo_pos);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));
    }

    // zurück zu Start und parken (mit Pause vor Auskoppeln)
    goto_index(0);
    *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
    ESP_LOGI(TAG,"Targets fertig: Pos1 erreicht, Servo hinten (Park/Start).");
}

/** Alle Wagen nacheinander abfahren (Testmodus). */
static void run_cycle_all(uint32_t *p_servo_pos){
    if(!p_servo_pos) return;

    if(pos_count < 1) pos_count = 1;
    if(pos_count > MAX_POS) pos_count = MAX_POS;

    *p_servo_pos = servo_to_front(*p_servo_pos);
    vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));

    for(int idx = 0; idx < pos_count; ++idx){
        goto_index(idx);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_BEFORE_MOVE));

        *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));

        dispense_one_full_rev();
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_DISP));

        *p_servo_pos = servo_to_front(*p_servo_pos);
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_COUPLE));
    }

    goto_index(0);
    *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
    ESP_LOGI(TAG,"Zyklus fertig: Pos1 erreicht, Servo hinten (Park/Start).");
}

// =================== Demo-Listen (Simulation AI-Output) ===================
static const char *RECIPE_A[] = { "salz", "pfeffer", "paprika", "chiliflocken" }; // "chiliflocken" fehlt absichtlich
static const int   RECIPE_A_LEN = sizeof(RECIPE_A)/sizeof(RECIPE_A[0]);

static const char *RECIPE_B[] = { "rosmarin", "paprika", "salz" };
static const int   RECIPE_B_LEN = sizeof(RECIPE_B)/sizeof(RECIPE_B[0]);

// =================== Main ===================
void app_main(void){
    // IO
    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL<<STEP_PIN)|(1ULL<<DIR_PIN)|(1ULL<<EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_out);
    gpio_set_level(EN_PIN,0); // Enable LOW
    esp_rom_delay_us(5000);

    gpio_config_t io_btn = {
        .pin_bit_mask = (1ULL<<BTN_STEP)|(1ULL<<BTN_SERVO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_btn);
    deb_state_t db_cycle = {.last_level=1,.stable_level=1,.stable_count=0};
    deb_state_t db_servo = {.last_level=1,.stable_level=1,.stable_count=0};

    // Servo PWM
    ledc_timer_config_t tcfg = {
        .speed_mode       = SERVO_TIMER_MODE,
        .duty_resolution  = SERVO_DUTY_RES,
        .timer_num        = SERVO_TIMER,
        .freq_hz          = SERVO_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t c0 = {
        .gpio_num   = SERVO_PIN,
        .speed_mode = SERVO_TIMER_MODE,
        .channel    = SERVO_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SERVO_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&c0);

    // Startzustand
    pos_count = POS_COUNT_DEFAULT; // ggf. später per UI/Serial veränderbar
    current_index = 0;             // Pos1
    uint32_t servo_pos = SERVO_BACK_US; // Park/Start: hinten
    servo_write_us(servo_pos);
    abs_steps = 0;

    // Regal-Mapping bemustern (kannst du später dynamisch setzen)
    spice_clear_all();
    spice_set(0, "salz");
    spice_set(1, "pfeffer");
    spice_set(2, "rosmarin");
    spice_set(3, "paprika");
    // weitere Plätze bleiben leer ("") und werden ignoriert

    ESP_LOGI(TAG,"Bereit. POS_COUNT=%d, TRAVEL=%d, DISP_REV=%d",
             pos_count, TRAVEL_STEPS_PER_STOP, DISPENSE_STEPS_PER_REV);

    // Demo: per Tastendruck wird zwischen Liste A und B gewechselt
    int which_list = 0;
    int targets[MAX_POS];

    while(1){
        if(edge_falling(BTN_STEP,&db_cycle)){
            const char **list = (which_list==0)? RECIPE_A : RECIPE_B;
            int list_len      = (which_list==0)? RECIPE_A_LEN : RECIPE_B_LEN;
            ESP_LOGI(TAG,"BTN_STEP -> Rezeptliste %s", (which_list==0)?"A":"B");

            // Abgleich AI-Liste -> Ziel-Wagen (ohne Duplikate)
            int n_targets = build_targets_from_spices(list, list_len, targets, MAX_POS, true);

            if(n_targets>0){
                run_cycle_targets(&servo_pos, targets, n_targets);
            } else {
                ESP_LOGW(TAG,"Keine passenden Gewürze im Regal -> kein Lauf.");
            }

            which_list ^= 1; // beim nächsten Druck die andere Liste
        }

        if(edge_falling(BTN_SERVO,&db_servo)){
            ESP_LOGI(TAG,"BTN_SERVO -> Servo Toggle");
            if(servo_pos==SERVO_BACK_US) servo_pos = servo_to_front(servo_pos);
            else                          servo_pos = decouple_to_back_with_pause(servo_pos); // Pause vor Auskoppeln
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
