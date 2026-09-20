#include "meet_ota.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace meet {
namespace {

constexpr char TAG[] = "meet_ota";
constexpr size_t kChunk = 4096;

struct OtaJob {
    MeetFirmwareInfo info;
};

std::atomic<bool> busy_{false};

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseSha256(const std::string& hex, uint8_t out[32]) {
    if (hex.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        const int hi = HexNibble(hex[i * 2]);
        const int lo = HexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void OtaTask(void* arg) {
    std::unique_ptr<OtaJob> job(static_cast<OtaJob*>(arg));
    const MeetFirmwareInfo& info = job->info;
    ESP_LOGI(TAG, "OTA start %s (%u bytes)", info.version.c_str(),
             static_cast<unsigned>(info.size));

    const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
    if (!dest) {
        ESP_LOGE(TAG, "no OTA partition");
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t http = {};
    http.url = info.url.c_str();
    http.timeout_ms = 30000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.keep_alive_enable = true;

    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(dest, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    mbedtls_md_context_t sha;
    mbedtls_md_init(&sha);
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info || mbedtls_md_setup(&sha, md_info, 0) != 0 || mbedtls_md_starts(&sha) != 0) {
        ESP_LOGE(TAG, "sha256 init failed");
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        mbedtls_md_free(&sha);
        busy_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    std::vector<char> buf(kChunk);
    int total = 0;
    while (true) {
        const int n = esp_http_client_read(client, buf.data(), static_cast<int>(buf.size()));
        if (n < 0) {
            ESP_LOGE(TAG, "http read failed");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        err = esp_ota_write(handle, buf.data(), n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write: %s", esp_err_to_name(err));
            break;
        }
        mbedtls_md_update(&sha, reinterpret_cast<const unsigned char*>(buf.data()),
                          static_cast<size_t>(n));
        total += n;
    }

    uint8_t digest[32] = {};
    mbedtls_md_finish(&sha, digest);
    mbedtls_md_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    uint8_t expect[32] = {};
    if (err == ESP_OK && ParseSha256(info.sha256, expect) &&
        memcmp(digest, expect, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "sha256 mismatch after %d bytes", total);
        err = ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_OK) {
        err = esp_ota_end(handle);
    } else {
        esp_ota_abort(handle);
    }

    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(dest);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA ok %d bytes, restart", total);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    busy_.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

bool MeetOtaBusy() {
    return busy_.load();
}

esp_err_t MeetOtaStart(const MeetFirmwareInfo& info) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (info.url.empty()) {
        busy_.store(false);
        return ESP_ERR_INVALID_ARG;
    }
    auto* job = new OtaJob{info};
    const BaseType_t ok = xTaskCreate(OtaTask, "meet_ota", 8192, job, 5, nullptr);
    if (ok != pdPASS) {
        delete job;
        busy_.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace meet
