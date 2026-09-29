#include "boot_button.h"
#include "board_display.h"
#include "cpu_load_led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "openwrt_status.h"
#include "status_dashboard.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define DISPLAY_DIM_DELAY_MS 60000
#define CLOCK_DISPLAY_DELAY_MS 10000

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(board_display_init());
    ESP_ERROR_CHECK(cpu_load_led_start());
    TickType_t display_started = xTaskGetTickCount();
    bool display_dimmed = false;
    bool clock_shown = false;
    status_dashboard_init();
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(openwrt_status_start());
    boot_button_start();

    bool connected = false;
    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Saved Wi-Fi configuration found; connecting for up to %d seconds",
                 CONFIG_PROV_CONNECT_TIMEOUT_SECONDS);
        ESP_ERROR_CHECK(wifi_manager_connect_saved());

        for (int i = 0; i < CONFIG_PROV_CONNECT_TIMEOUT_SECONDS * 10; ++i) {
            if (wifi_manager_get_state() == WIFI_MANAGER_CONNECTED) {
                ESP_LOGI(TAG, "Connected using saved configuration");
                connected = true;
                break;
            }
            if (!display_dimmed && xTaskGetTickCount() - display_started >=
                                      pdMS_TO_TICKS(DISPLAY_DIM_DELAY_MS)) {
                board_display_set_brightness(25);
                display_dimmed = true;
                ESP_LOGI(TAG, "Display brightness reduced to 25%%");
            }
            if (!clock_shown && xTaskGetTickCount() - display_started >=
                                pdMS_TO_TICKS(CLOCK_DISPLAY_DELAY_MS)) {
                status_dashboard_set_clock_visible(true);
                clock_shown = true;
            }
            if (status_dashboard_process_ui_requests()) {
                board_display_set_brightness(50);
                display_started = xTaskGetTickCount();
                display_dimmed = false;
                clock_shown = false;
                ESP_LOGI(TAG, "Manual page switch: brightness set to 50%%; dim timer reset");
            }
            status_dashboard_animate_frame();
            board_display_handle();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!connected) {
            ESP_LOGW(TAG, "Saved network connection timed out; opening provisioning portal");
        }
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi configuration; opening provisioning portal");
    }

    if (!connected) ESP_ERROR_CHECK(wifi_manager_start_provisioning());

    TickType_t last_update = 0;
    TickType_t last_frame_wake = xTaskGetTickCount();
    uint32_t frame_tick_remainder = 0;
    int64_t perf_window_started = esp_timer_get_time();
    uint64_t handler_total_us = 0;
    uint32_t handler_max_us = 0;
    uint32_t handler_calls = 0;
    uint32_t handler_over_budget = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!display_dimmed && now - display_started >= pdMS_TO_TICKS(DISPLAY_DIM_DELAY_MS)) {
            board_display_set_brightness(25);
            display_dimmed = true;
            ESP_LOGI(TAG, "Display brightness reduced to 25%%");
        }
        if (!clock_shown && now - display_started >= pdMS_TO_TICKS(CLOCK_DISPLAY_DELAY_MS)) {
            status_dashboard_set_clock_visible(true);
            clock_shown = true;
            ESP_LOGI(TAG, "Idle clock displayed");
        }
        if (now - last_update >= pdMS_TO_TICKS(500)) {
            status_dashboard_update();
            last_update = now;
        }
        if (status_dashboard_process_ui_requests()) {
            board_display_set_brightness(50);
            display_started = now;
            display_dimmed = false;
            clock_shown = false;
            ESP_LOGI(TAG, "Manual page switch: brightness set to 50%%; dim timer reset");
        }
        int64_t handler_started = esp_timer_get_time();
        status_dashboard_animate_frame();
        board_display_handle();
        uint32_t handler_us = (uint32_t)(esp_timer_get_time() - handler_started);
        handler_total_us += handler_us;
        if (handler_us > handler_max_us) handler_max_us = handler_us;
        if (handler_us > 16667) handler_over_budget++;
        handler_calls++;

        int64_t perf_now = esp_timer_get_time();
        if (perf_now - perf_window_started >= 10000000) {
            board_display_perf_t lcd_perf;
            status_dashboard_perf_t dashboard_perf;
            board_display_take_perf(&lcd_perf);
            status_dashboard_take_perf(&dashboard_perf);
            ESP_LOGI(TAG, "LVGL/10s: calls=%lu avg=%llu us max=%lu us >16.7ms=%lu",
                     (unsigned long)handler_calls,
                     (unsigned long long)(handler_calls ? handler_total_us / handler_calls : 0),
                     (unsigned long)handler_max_us,
                     (unsigned long)handler_over_budget);
            ESP_LOGI(TAG, "LCD/10s: page=%s refresh=%lu flush=%lu DMA=%lu tx=%lu KiB wave CPU/MEM/TEMP/D/U=%lu/%lu/%lu/%lu/%lu",
                     dashboard_perf.traffic_page_visible ? "traffic" : "default",
                     (unsigned long)lcd_perf.refreshes,
                     (unsigned long)lcd_perf.flushes,
                     (unsigned long)lcd_perf.dma_completed,
                     (unsigned long)(lcd_perf.pixels / 512U),
                     (unsigned long)dashboard_perf.liquid_updates[0],
                     (unsigned long)dashboard_perf.liquid_updates[1],
                     (unsigned long)dashboard_perf.liquid_updates[2],
                     (unsigned long)dashboard_perf.liquid_updates[3],
                     (unsigned long)dashboard_perf.liquid_updates[4]);
            perf_window_started = perf_now;
            handler_total_us = 0;
            handler_max_us = 0;
            handler_calls = 0;
            handler_over_budget = 0;
        }
        /* Fractional tick scheduler: at the default 100 Hz FreeRTOS tick this
         * alternates 1/2/2 ticks, averaging exactly 60 handler calls/second. */
        frame_tick_remainder += configTICK_RATE_HZ;
        TickType_t frame_ticks = frame_tick_remainder / 60;
        frame_tick_remainder %= 60;
        if (frame_ticks == 0) frame_ticks = 1;
        xTaskDelayUntil(&last_frame_wake, frame_ticks);
    }
}
