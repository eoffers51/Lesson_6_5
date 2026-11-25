/*
 * Lesson 6.5 - Multicore, ISRs, shared resources (ESP32-S3)
 *
 * Button 1 (GPIO4)            -> ISR -> parts counter (+1)
 * Button 2 (GPIO5) or BOOT(0) -> ISR -> wakes the E-stop task
 * Task 1 (core 0, 1 s)        -> counter -> speed -> formats the report
 * Task 2 (core 1)             -> ADC     -> appends to the report, prints it
 * Task 3 (highest priority)   -> LED on, "SYSTEM HALTED!", conveyor blocked
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "conveyor";

/* ---- Pins ---- */
#define PIN_BTN_PARTS GPIO_NUM_4
#define PIN_BTN_ESTOP GPIO_NUM_5
#define PIN_BTN_BOOT GPIO_NUM_0
#define PIN_LED_ALARM GPIO_NUM_2
#define ADC_CHANNEL ADC_CHANNEL_0 /* GPIO1 on the ESP32-S3 */

/* ---- Timing ---- */
#define REPORT_PERIOD_MS 1000
#define DEBOUNCE_MS 50
#define MUTEX_WAIT_MS 100

/* ---- Task setup ---- */
#define STACK_SIZE 4096
#define PRIO_REPORT 5
#define PRIO_ADC 5
#define PRIO_ESTOP (configMAX_PRIORITIES - 1)

/* ---- Shared resource 1: counter + halt flag (ISR <-> tasks, both cores) ---- */
static portMUX_TYPE s_counter_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_part_count = 0;
static volatile bool s_halted = false;

/* ---- Shared resource 2: report buffer (tasks only, never an ISR) ---- */
#define REPORT_LEN 160
static char s_report[REPORT_LEN];
static SemaphoreHandle_t s_report_mutex;

static TaskHandle_t s_adc_task;
static TaskHandle_t s_estop_task;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali; /* NULL if no calibration data */

/* ================= ISRs: short, no logging, no mutex, no blocking ================= */

/* Debounce state of one button. Each pin has its own, passed as the ISR arg. */
typedef struct {
    TickType_t last_edge;
    bool pressed; /* starts released: the pull-up keeps the pin HIGH */
} debounce_t;

static debounce_t s_db_parts, s_db_estop, s_db_boot;

/* Called from an ISR that fires on BOTH edges. The first edge after
 * DEBOUNCE_MS of quiet starts a new transition and flips the stable state
 * (released <-> pressed). Every edge restarts the quiet timer, so bounce
 * edges on press AND on release are ignored. Returns true only for the flip
 * to "pressed", on the very first edge, so there is no extra delay. The pin
 * level is not read: during bounce it may already have changed again. */
static inline bool IRAM_ATTR debounce_is_press(debounce_t *db)
{
    TickType_t now = xTaskGetTickCountFromISR();
    bool quiet = (now - db->last_edge) >= pdMS_TO_TICKS(DEBOUNCE_MS);
    db->last_edge = now;
    if (!quiet) {
        return false; /* bounce */
    }
    db->pressed = !db->pressed;
    return db->pressed; /* false = release */
}

static void IRAM_ATTR parts_isr(void *arg)
{
    if (!debounce_is_press(arg)) {
        return;
    }
    portENTER_CRITICAL_ISR(&s_counter_lock);
    if (!s_halted) {
        s_part_count++;
    }
    portEXIT_CRITICAL_ISR(&s_counter_lock);
}

static void IRAM_ATTR estop_isr(void *arg)
{
    if (!debounce_is_press(arg)) {
        return;
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_estop_task, &woken);
    portYIELD_FROM_ISR(woken); /* switch to Task 3 right after the ISR */
}

/* ================= Tasks ================= */

static uint32_t read_counter(bool *halted)
{
    portENTER_CRITICAL(&s_counter_lock);
    uint32_t count = s_part_count;
    *halted = s_halted;
    portEXIT_CRITICAL(&s_counter_lock);
    return count;
}

/* Task 1, core 0: counter -> speed -> format the report */
static void report_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint32_t prev = 0;

    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(REPORT_PERIOD_MS));

        bool halted;
        uint32_t count = read_counter(&halted);
        if (halted) {
            continue; /* conveyor blocked: leave "SYSTEM HALTED!" alone */
        }
        uint32_t per_min = (count - prev) * (60000 / REPORT_PERIOD_MS);
        prev = count;

        if (xSemaphoreTake(s_report_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "T1: report buffer busy, skipped");
            continue;
        }
        /* Re-check under the mutex: Task 3 may have halted us in between */
        if (!s_halted) {
            snprintf(s_report, sizeof(s_report), "[core %d] parts=%" PRIu32 " speed=%" PRIu32 "/min",
                     xPortGetCoreID(), count, per_min);
        }
        xSemaphoreGive(s_report_mutex);

        xTaskNotifyGive(s_adc_task); /* "report formatted, append your part" */
    }
}

/* Task 2, core 1: ADC -> append to the report -> print the whole line */
static void adc_task(void *arg)
{
    for (;;) {
        /* Woken once per second by Task 1, so the order is always format, then append */
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0) {
            continue;
        }

        int raw = 0, mv = -1;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL, &raw) != ESP_OK) {
            raw = -1;
        } else if (s_adc_cali) {
            adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
        }

        char line[REPORT_LEN];
        if (xSemaphoreTake(s_report_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "T2: report buffer busy, skipped");
            continue;
        }
        if (!s_halted) {
            size_t used = strlen(s_report);
            snprintf(s_report + used, sizeof(s_report) - used, " | [core %d] adc=%d raw %d mV",
                     xPortGetCoreID(), raw, mv);
        }
        memcpy(line, s_report, sizeof(line)); /* copy out, print outside the lock */
        xSemaphoreGive(s_report_mutex);

        if (!s_halted) {
            printf("%s\n", line);
        }
    }
}

/* Task 3, highest priority: sleeps until the E-stop ISR wakes it */
static void estop_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* 0 % CPU while waiting */

        if (s_halted) {
            /* A second press changes nothing. It only shows the frozen counter,
             * which proves that BTN1 no longer counts (used by the Wokwi test). */
            bool halted;
            printf("already halted, parts=%" PRIu32 "\n", read_counter(&halted));
            continue;
        }

        /* 1. Block the conveyor first: the ISR and Tasks 1/2 check this flag */
        portENTER_CRITICAL(&s_counter_lock);
        s_halted = true;
        uint32_t count = s_part_count;
        portEXIT_CRITICAL(&s_counter_lock);
        gpio_intr_disable(PIN_BTN_PARTS);

        /* 2. Alarm LED */
        gpio_set_level(PIN_LED_ALARM, 1);

        /* 3. Safe write: wait as long as needed, a halt message must not be lost */
        xSemaphoreTake(s_report_mutex, portMAX_DELAY);
        strlcpy(s_report, "SYSTEM HALTED!", sizeof(s_report));
        xSemaphoreGive(s_report_mutex);

        printf("SYSTEM HALTED! (core %d, parts=%" PRIu32 ")\n", xPortGetCoreID(), count);
    }
}

/* ================= Setup ================= */

static esp_err_t setup_gpio(void)
{
    gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << PIN_BTN_PARTS) | (1ULL << PIN_BTN_ESTOP) | (1ULL << PIN_BTN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* button pulls to GND */
        .intr_type = GPIO_INTR_ANYEDGE,   /* both edges: see debounce_is_press() */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&buttons), TAG, "buttons");

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_LED_ALARM,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "led");
    gpio_set_level(PIN_LED_ALARM, 0);

    /* ESP_ERR_INVALID_STATE = service already installed (e.g. by the Arduino core in Wokwi) */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_BTN_PARTS, parts_isr, &s_db_parts), TAG, "parts isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_BTN_ESTOP, estop_isr, &s_db_estop), TAG, "estop isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_BTN_BOOT, estop_isr, &s_db_boot), TAG, "boot isr");
    return ESP_OK;
}

static esp_err_t setup_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL, &chan), TAG, "adc chan");

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration in eFuse, printing raw only (mV = -1)");
        s_adc_cali = NULL;
    }
    return ESP_OK;
}

void app_main(void)
{
    s_report_mutex = xSemaphoreCreateMutex();
    if (s_report_mutex == NULL || setup_adc() != ESP_OK) {
        ESP_LOGE(TAG, "setup failed");
        return;
    }

    /* Create the receivers before the ISRs and Task 1 can notify them */
    if (xTaskCreate(estop_task, "estop", STACK_SIZE, NULL, PRIO_ESTOP, &s_estop_task) != pdPASS ||
        xTaskCreatePinnedToCore(adc_task, "adc", STACK_SIZE, NULL, PRIO_ADC, &s_adc_task, 1) != pdPASS ||
        setup_gpio() != ESP_OK ||
        xTaskCreatePinnedToCore(report_task, "report", STACK_SIZE, NULL, PRIO_REPORT, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task/gpio setup failed");
        return;
    }

    ESP_LOGI(TAG, "running: BTN1=GPIO%d parts, BTN2=GPIO%d or BOOT=GPIO%d e-stop, LED=GPIO%d, ADC=GPIO1",
             PIN_BTN_PARTS, PIN_BTN_ESTOP, PIN_BTN_BOOT, PIN_LED_ALARM);
}
