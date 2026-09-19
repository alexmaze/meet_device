#pragma once

#include "meet_config.h"

#include <esp_err.h>

namespace meet {

esp_err_t MeetNvsLoad(MeetConfig& out);
esp_err_t MeetNvsSave(const MeetConfig& in);

}  // namespace meet
