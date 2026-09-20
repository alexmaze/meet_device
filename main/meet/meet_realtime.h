#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <functional>
#include <string>

namespace meet {

/** Fixed to match web QwenWebSocketRealtimeClient (runtime HTTP has no maxHistoryTurns). */
constexpr int kMeetMaxHistoryTurns = 50;

struct MeetRealtimeSessionConfig {
    std::string voice;
    std::string instructions;
    int max_history_turns = kMeetMaxHistoryTurns;
};

using MeetRealtimeAudioDeltaCb =
    std::function<void(const std::string& response_id, const uint8_t* pcm, size_t bytes)>;
using MeetRealtimeSpeechStartedCb = std::function<void()>;
using MeetRealtimeActivityCb = std::function<void()>;
using MeetRealtimeDisconnectedCb = std::function<void()>;
using MeetRealtimeEmotionCb = std::function<void(const char* name)>;
using MeetRealtimeCaptionCb = std::function<void(const char* text, bool done)>;
using MeetRealtimeReadyCb = std::function<void()>;
using MeetRealtimeTeachingGateCb = std::function<void(int revision, bool open)>;

/**
 * Meet duplex realtime WebSocket client.
 * Path: /api/characters/:id/realtime/websocket?conversationId=
 * Does NOT depend on audio/ — callers wire audio via SetAudioDeltaHandler / speech callbacks.
 *
 * Qwen relay does NOT accept input_audio_buffer.commit — never send it.
 */
class MeetRealtime {
public:
    static MeetRealtime& Instance();

    void SetAudioDeltaHandler(MeetRealtimeAudioDeltaCb cb);
    void SetSpeechStartedHandler(MeetRealtimeSpeechStartedCb cb);
    void SetActivityHandler(MeetRealtimeActivityCb cb);
    void SetDisconnectedHandler(MeetRealtimeDisconnectedCb cb);
    void SetEmotionHandler(MeetRealtimeEmotionCb cb);
    void SetCaptionHandler(MeetRealtimeCaptionCb cb);
    void SetReadyHandler(MeetRealtimeReadyCb cb);
    void SetTeachingGateHandler(MeetRealtimeTeachingGateCb cb);

    esp_err_t Open(const std::string& origin,
                   const std::string& bearer_token,
                   const std::string& character_id,
                   const std::string& conversation_id,
                   const MeetRealtimeSessionConfig& session);
    void Close();
    bool IsReady() const { return ready_; }
    bool IsConnected() const { return connected_; }
    bool TeachingGateOpen() const { return teaching_gate_open_.load(); }
    void AssumeRelayReady();

    esp_err_t SendSessionUpdate();
    /** Uplink 16-bit PCM mono; base64 in input_audio_buffer.append. 40ms send timeout; drops on stall. */
    esp_err_t SendInputPcm(const int16_t* samples, size_t sample_count);
    esp_err_t SendResponseCancel();
    esp_err_t SendTeachingAudioGateAck(int revision);

private:
    MeetRealtime() = default;

    static void WebsocketEventHandler(void* handler_args,
                                      esp_event_base_t base,
                                      int32_t event_id,
                                      void* event_data);
    void OnMessage(const char* data, int len);
    esp_err_t SendJson(const char* json, size_t len, TickType_t timeout);

    void* client_ = nullptr;
    bool connected_ = false;
    bool relay_ready_ = false;
    bool ready_ = false;
    bool session_sent_ = false;
    bool teaching_session_ = false;
    std::atomic<bool> teaching_gate_open_{true};
    std::string auth_header_;
    MeetRealtimeSessionConfig session_;
    std::string uplink_json_;  // reused buffer for append frames

    MeetRealtimeAudioDeltaCb on_audio_delta_;
    MeetRealtimeSpeechStartedCb on_speech_started_;
    MeetRealtimeActivityCb on_activity_;
    MeetRealtimeDisconnectedCb on_disconnected_;
    MeetRealtimeEmotionCb on_emotion_;
    MeetRealtimeCaptionCb on_caption_;
    MeetRealtimeReadyCb on_ready_;
    MeetRealtimeTeachingGateCb on_teaching_gate_;
};

}  // namespace meet
