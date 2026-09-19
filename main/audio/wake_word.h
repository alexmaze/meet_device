#pragma once

#include <functional>
#include <esp_err.h>

namespace meet {

using WakeWordDetectedCb = std::function<void()>;

/**
 * Wake-word interface stub. Boot button path works without this.
 * TODO: wire espressif/esp-sr AFE when product enables wake.
 */
class WakeWord {
public:
    static WakeWord& Instance();

    esp_err_t Start();
    void Stop();
    void SetOnDetected(WakeWordDetectedCb cb);

private:
    WakeWord() = default;
    bool running_ = false;
    WakeWordDetectedCb on_detected_;
};

}  // namespace meet
