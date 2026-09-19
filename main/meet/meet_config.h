#pragma once

#include <string>

namespace meet {

/** NVS key names for Meet device config. */
struct MeetNvsKeys {
    static constexpr const char* kNamespace = "meet";
    static constexpr const char* kServerOrigin = "srv_origin";
    static constexpr const char* kDeviceCredential = "dev_cred";
    static constexpr const char* kDeviceId = "dev_id";
    static constexpr const char* kCharacterId = "char_id";
    static constexpr const char* kCharacterName = "char_name";
    static constexpr const char* kLandscape = "landscape";
    static constexpr const char* kVolume = "volume";
};

struct MeetConfig {
    std::string server_origin;
    std::string device_credential;
    std::string device_id;
    std::string selected_character_id;
    std::string selected_character_name;
    bool landscape = false;
    int volume = 70;
};

}  // namespace meet
