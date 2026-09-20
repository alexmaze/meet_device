#pragma once

#include <esp_err.h>
#include <string>

namespace meet {

/** Write-once factory serial, persisted in NVS namespace `factory`. */
esp_err_t FactorySerialLoad(std::string& out);

}  // namespace meet
