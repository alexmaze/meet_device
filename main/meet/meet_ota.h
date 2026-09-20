#pragma once

#include "meet_api.h"

#include <esp_err.h>
#include <string>

namespace meet {

bool MeetOtaBusy();
esp_err_t MeetOtaStart(const MeetFirmwareInfo& info);

}  // namespace meet
