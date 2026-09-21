#include "app_controller.h"
#include "app_event.h"
#include "audio_pipeline.h"
#include "board.h"
#include "ui.h"
#include "wifi_service.h"

#include <sdkconfig.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr char TAG[] = "main";

void UiTask(void* /*arg*/) {
    constexpr uint32_t kPeriodMs = 20;
    while (true) {
        meet::UiTick(kPeriodMs);
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    }
}

void AppTask(void* /*arg*/) {
    meet::AppController::Instance().RunLoop();
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Meet companion firmware %s", CONFIG_MEET_FIRMWARE_VERSION);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(meet::AppEventQueueInit());

    ESP_ERROR_CHECK(meet::Board::Instance().Init());
    ESP_ERROR_CHECK(meet::UiInit());
    // Wi-Fi must come up before AFE: the wifi task needs internal RAM that AFE would consume.
    ESP_ERROR_CHECK(meet::WifiService::Instance().Init());
    ESP_ERROR_CHECK(meet::AudioPipeline::Instance().Init());
    ESP_ERROR_CHECK(meet::AppController::Instance().Start());

    xTaskCreate(UiTask, "ui", 6144, nullptr, 5, nullptr);
    xTaskCreate(AppTask, "app", 12288, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "running");
}
