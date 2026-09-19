#include "afe_processor.h"

#include "sr_models.h"

#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <string>

namespace meet {
namespace {

constexpr char TAG[] = "afe_proc";
constexpr EventBits_t kRunning = 0x01;

}  // namespace

AfeProcessor& AfeProcessor::Instance() {
    static AfeProcessor proc;
    return proc;
}

esp_err_t AfeProcessor::Init(int channels, bool input_reference) {
    if (ready_) {
        return ESP_OK;
    }
    channels_ = channels > 0 ? channels : 2;
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        return ESP_ERR_NO_MEM;
    }

    int ref_num = input_reference ? 1 : 0;
    std::string fmt;
    for (int i = 0; i < channels_ - ref_num; ++i) {
        fmt.push_back('M');
    }
    for (int i = 0; i < ref_num; ++i) {
        fmt.push_back('R');
    }

    afe_config_t* cfg = afe_config_init(fmt.c_str(), SrModels(), AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGW(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    cfg->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
#if CONFIG_MEET_USE_DEVICE_AEC
    cfg->aec_init = true;
    cfg->vad_init = false;
#else
    cfg->aec_init = false;
    cfg->vad_init = true;
#endif
    cfg->agc_init = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    auto* iface = esp_afe_handle_from_config(cfg);
    if (!iface) {
        ESP_LOGW(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }
    afe_iface_ = iface;
    afe_data_ = iface->create_from_config(cfg);
    if (!afe_data_) {
        ESP_LOGW(TAG, "create_from_config failed");
        return ESP_FAIL;
    }
    xTaskCreate(Task, "afe_proc", 4096, this, 3, nullptr);
    ready_ = true;
    ESP_LOGI(TAG, "AFE VC ready fmt=%s aec=%d", fmt.c_str(), cfg->aec_init ? 1 : 0);
    return ESP_OK;
}

void AfeProcessor::Start() {
    if (event_group_) {
        xEventGroupSetBits(static_cast<EventGroupHandle_t>(event_group_), kRunning);
    }
}

void AfeProcessor::Stop() {
    if (event_group_) {
        xEventGroupClearBits(static_cast<EventGroupHandle_t>(event_group_), kRunning);
    }
    std::lock_guard<std::mutex> lock(feed_mu_);
    auto* iface = static_cast<const esp_afe_sr_iface_t*>(afe_iface_);
    auto* data = static_cast<esp_afe_sr_data_t*>(afe_data_);
    if (iface && data) {
        iface->reset_buffer(data);
    }
    feed_buf_.clear();
}

bool AfeProcessor::running() const {
    if (!event_group_) {
        return false;
    }
    return xEventGroupGetBits(static_cast<EventGroupHandle_t>(event_group_)) & kRunning;
}

void AfeProcessor::FeedInterleaved16k(const int16_t* data, size_t samples) {
    auto* iface = static_cast<const esp_afe_sr_iface_t*>(afe_iface_);
    auto* afe = static_cast<esp_afe_sr_data_t*>(afe_data_);
    if (!iface || !afe || !data || samples == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(feed_mu_);
    if (!running()) {
        return;
    }
    feed_buf_.insert(feed_buf_.end(), data, data + samples);
    const size_t chunk = static_cast<size_t>(iface->get_feed_chunksize(afe)) * static_cast<size_t>(channels_);
    if (chunk == 0) {
        return;
    }
    while (feed_buf_.size() >= chunk) {
        iface->feed(afe, feed_buf_.data());
        feed_buf_.erase(feed_buf_.begin(), feed_buf_.begin() + static_cast<std::ptrdiff_t>(chunk));
    }
}

void AfeProcessor::SetOutputHandler(std::function<void(const int16_t* data, size_t samples)> cb) {
    on_output_ = std::move(cb);
}

void AfeProcessor::Task(void* arg) {
    auto* self = static_cast<AfeProcessor*>(arg);
    auto* iface = static_cast<const esp_afe_sr_iface_t*>(self->afe_iface_);
    auto* afe = static_cast<esp_afe_sr_data_t*>(self->afe_data_);
    while (true) {
        xEventGroupWaitBits(static_cast<EventGroupHandle_t>(self->event_group_), kRunning, pdFALSE,
                            pdTRUE, portMAX_DELAY);
        auto* res = iface->fetch_with_delay(afe, portMAX_DELAY);
        if (!self->running() || !res || res->ret_value == ESP_FAIL) {
            continue;
        }
        if (self->on_output_ && res->data && res->data_size > 0) {
            self->on_output_(res->data, res->data_size / sizeof(int16_t));
        }
    }
}

}  // namespace meet
