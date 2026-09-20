#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdint>

namespace meet {

enum class AppEventType : uint8_t {
    BootClick,
    BootLongPress,
    VolumeKey,
    WakeDetected,
    WifiPhaseChanged,
    WifiDropped,
    WifiConnectRequested,  // SoftAP form saved credentials
    RtReady,
    RtDisconnected,
    RtSpeechStarted,
    RtEmotion,
    RtCaption,
    RtCaptionDone,
    RtTeachingGate,
    Unauthorized,
};

struct AppEvent {
    AppEventType type = AppEventType::BootClick;
    int32_t i32 = 0;
    char text[96] = {};
};

/** Global control-plane queue. Producers only xQueueSend; app task is the sole consumer. */
QueueHandle_t AppEventQueue();
esp_err_t AppEventQueueInit();
bool AppEventPost(const AppEvent& ev);
bool AppEventPostFromIsr(const AppEvent& ev);

}  // namespace meet
