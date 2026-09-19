#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <esp_event.h>
#include <functional>
#include <string>

namespace meet {

struct MeetRealtimeSessionConfig {
    std::string voice;
    std::string instructions;
    int max_history_turns = 50;
};

using MeetRealtimeAudioDeltaCb =
    std::function<void(const std::string& response_id, const uint8_t* pcm, size_t bytes)>;
using MeetRealtimeSpeechStartedCb = std::function<void()>;
using MeetRealtimeActivityCb = std::function<void()>;
using MeetRealtimeDisconnectedCb = std::function<void()>;

/**
 * Meet duplex realtime WebSocket client.
 * Path: /api/characters/:id/realtime/websocket?conversationId=
 */
class MeetRealtime {
public:
    static MeetRealtime& Instance();

    void SetAudioDeltaHandler(MeetRealtimeAudioDeltaCb cb);
    void SetSpeechStartedHandler(MeetRealtimeSpeechStartedCb cb);
    void SetActivityHandler(MeetRealtimeActivityCb cb);
    void SetDisconnectedHandler(MeetRealtimeDisconnectedCb cb);

    esp_err_t Open(const std::string& character_id,
                   const std::string& conversation_id,
                   const MeetRealtimeSessionConfig& session);
    void Close();
    bool IsReady() const { return ready_; }
    bool IsConnected() const { return connected_; }
    void AssumeRelayReady();

    /** Send session.update after relay.ready (or immediately if no relay gate). */
    esp_err_t SendSessionUpdate();

    /** Uplink 16-bit PCM samples (mono); encoded base64 in input_audio_buffer.append. */
    esp_err_t SendInputPcm(const int16_t* samples, size_t sample_count);

    esp_err_t SendResponseCancel();

private:
    MeetRealtime() = default;

    static void WebsocketEventHandler(void* handler_args,
                                      esp_event_base_t base,
                                      int32_t event_id,
                                      void* event_data);
    void OnMessage(const char* data, int len);
    esp_err_t SendJson(const std::string& json);

    void* client_ = nullptr;  // esp_websocket_client_handle_t
    bool connected_ = false;
    bool relay_ready_ = false;
    bool ready_ = false;
    bool session_sent_ = false;
    std::string auth_header_;  // must outlive websocket handshake
    MeetRealtimeSessionConfig session_;
    MeetRealtimeAudioDeltaCb on_audio_delta_;
    MeetRealtimeSpeechStartedCb on_speech_started_;
    MeetRealtimeActivityCb on_activity_;
    MeetRealtimeDisconnectedCb on_disconnected_;
};

}  // namespace meet
