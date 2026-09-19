#include "sr_models.h"

#include <esp_log.h>
#include <model_path.h>

namespace meet {
namespace {
constexpr char TAG[] = "sr_models";
}

srmodel_list_t* SrModels() {
    static srmodel_list_t* models = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        models = esp_srmodel_init("model");
        if (!models || models->num <= 0) {
            ESP_LOGW(TAG, "no SR models in `model` partition; wake/AEC stay optional");
            models = nullptr;
        } else {
            ESP_LOGI(TAG, "loaded %d SR models", models->num);
        }
    }
    return models;
}

}  // namespace meet
