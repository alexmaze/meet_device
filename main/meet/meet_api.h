#pragma once

#include <esp_err.h>
#include <string>
#include <vector>

namespace meet {

struct MeetPairingSession {
    std::string id;
    std::string code;
};

struct MeetPairingPollResult {
    bool claimed = false;
    std::string device_credential;
    std::string device_id;
};

struct MeetCharacter {
    std::string id;
    std::string name;
};

struct MeetConversation {
    std::string id;
};

struct MeetAuthMe {
    std::string account_type;  // admin | adult | child
    std::string user_id;
};

struct MeetCharacterRuntime {
    std::string character_id;
    std::string voice;
    std::string instructions;
    std::string provider;  // qwen | doubao
    int max_history_turns = 50;
};

class MeetApi {
public:
    static MeetApi& Instance();

    void Configure(const std::string& server_origin, const std::string& bearer_token);

    esp_err_t CreatePairingSession(MeetPairingSession& out);
    esp_err_t PollPairingSession(const std::string& session_id, MeetPairingPollResult& out);
    esp_err_t ListCharacters(std::vector<MeetCharacter>& out);
    esp_err_t GetCharacterRuntime(const std::string& character_id, MeetCharacterRuntime& out);
    esp_err_t UpdateSelectedCharacter(const std::string& character_id);
    esp_err_t CreateConversation(const std::string& character_id,
                                 const std::string& mode,
                                 MeetConversation& out);
    esp_err_t TeachingPrepareChatOnly(const std::string& conversation_id);
    esp_err_t CompleteConversation(const std::string& conversation_id, int last_sequence);
    esp_err_t GetAuthMe(MeetAuthMe& out);

private:
    MeetApi() = default;

    esp_err_t HttpJson(const char* method,
                       const std::string& path,
                       const char* body_json,
                       std::string& response_body,
                       int* status_out);

    std::string origin_;
    std::string bearer_;
};

}  // namespace meet
