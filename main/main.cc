#include "app_controller.h"
#include "board.h"
#include "pcm_pipeline.h"
#include "ui.h"
#include "wake_word.h"

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

void AppTickTask(void* /*arg*/) {
    while (true) {
        meet::AppController::Instance().Tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Meet companion firmware 0.1.0");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(meet::Board::Instance().Init());
    ESP_ERROR_CHECK(meet::UiInit());
    ESP_ERROR_CHECK(meet::PcmPipeline::Instance().Init());
    meet::WakeWord::Instance().Start();

    ESP_ERROR_CHECK(meet::AppController::Instance().Start());

    xTaskCreate(UiTask, "ui", 8192, nullptr, 5, nullptr);
    xTaskCreate(AppTickTask, "app_tick", 8192, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "running");
}
