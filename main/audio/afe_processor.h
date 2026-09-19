#pragma once

#include <cstdint>
#include <esp_err.h>
#include <functional>
#include <mutex>
#include <vector>

namespace meet {

class AfeProcessor {
public:
    static AfeProcessor& Instance();

    esp_err_t Init(int channels, bool input_reference);
    void Start();
    void Stop();
    bool running() const;
    bool ready() const { return ready_; }

    void FeedInterleaved16k(const int16_t* data, size_t samples);
    void SetOutputHandler(std::function<void(const int16_t* data, size_t samples)> cb);

private:
    AfeProcessor() = default;
    static void Task(void* arg);

    bool ready_ = false;
    int channels_ = 2;
    void* event_group_ = nullptr;
    const void* afe_iface_ = nullptr;
    void* afe_data_ = nullptr;
    std::function<void(const int16_t*, size_t)> on_output_;
    std::mutex feed_mu_;
    std::vector<int16_t> feed_buf_;
};

}  // namespace meet
