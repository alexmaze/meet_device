#include "app_event.h"

#include <esp_log.h>

namespace meet {
namespace {

constexpr char TAG[] = "app_evt";
constexpr int kQueueDepth = 32;
QueueHandle_t g_queue = nullptr;

}  // namespace

esp_err_t AppEventQueueInit() {
    if (g_queue) {
        return ESP_OK;
    }
    g_queue = xQueueCreate(kQueueDepth, sizeof(AppEvent));
    if (!g_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

QueueHandle_t AppEventQueue() {
    return g_queue;
}

bool AppEventPost(const AppEvent& ev) {
    if (!g_queue) {
        return false;
    }
    if (xQueueSend(g_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, drop type=%d", static_cast<int>(ev.type));
        return false;
    }
    return true;
}

bool AppEventPostFromIsr(const AppEvent& ev) {
    if (!g_queue) {
        return false;
    }
    BaseType_t woken = pdFALSE;
    const bool ok = xQueueSendFromISR(g_queue, &ev, &woken) == pdTRUE;
    if (woken) {
        portYIELD_FROM_ISR();
    }
    return ok;
}

}  // namespace meet
