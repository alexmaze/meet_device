#pragma once

#include <cstdint>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <functional>
#include <mutex>
#include <vector>

namespace meet {

enum class AfeUnitType {
    VoiceComm,  // AFE_TYPE_VC — InCall AEC / uplink
    WakeWord,   // AFE_TYPE_SR — Ready wake detection
};

using AfeOutputCb = std::function<void(const int16_t* data, size_t samples)>;
using AfeWakeCb = std::function<void()>;

/** Parameterized AFE wrapper shared by VC and wake-word paths. */
class AfeUnit {
public:
    AfeUnit() = default;

    esp_err_t Init(AfeUnitType type,
                   int channels,
                   bool input_reference,
                   bool enable_aec,
                   const char* task_name,
                   uint32_t stack_size,
                   UBaseType_t priority);
    void Start();
    void Stop();
    bool ready() const { return ready_; }
    bool running() const;

    void FeedInterleaved16k(const int16_t* data, size_t samples);
    void SetOutputHandler(AfeOutputCb cb);
    void SetWakeHandler(AfeWakeCb cb);

private:
    static void Task(void* arg);

    AfeUnitType type_ = AfeUnitType::VoiceComm;
    bool ready_ = false;
    int channels_ = 2;
    void* event_group_ = nullptr;
    const void* afe_iface_ = nullptr;
    void* afe_data_ = nullptr;
    AfeOutputCb on_output_;
    AfeWakeCb on_wake_;
    std::mutex feed_mu_;
    std::vector<int16_t> feed_buf_;
};

}  // namespace meet
