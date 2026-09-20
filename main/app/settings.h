#pragma once

#include <esp_err.h>
#include <mutex>
#include <string>

namespace meet {

struct SettingsData {
    std::string server_origin;
    std::string device_credential;
    std::string device_id;
    std::string selected_character_id;
    std::string selected_character_name;
    std::string account_type;  // admin | adult | child; cached at bind time
    int volume = 70;
};

/**
 * In-memory settings singleton. Load once at boot; reads are lock-free copies of
 * string fields under a short mutex; writes persist to NVS.
 */
class Settings {
public:
    static Settings& Instance();

    esp_err_t Load();
    esp_err_t Save();

    SettingsData Snapshot() const;
    void Apply(const SettingsData& data);

    std::string server_origin() const;
    std::string device_credential() const;
    std::string device_id() const;
    std::string selected_character_id() const;
    std::string selected_character_name() const;
    std::string account_type() const;
    int volume() const;

    void SetServerOrigin(const std::string& v);
    void SetDeviceCredential(const std::string& cred, const std::string& device_id);
    void ClearCredential();
    void SetSelectedCharacter(const std::string& id, const std::string& name);
    void SetAccountType(const std::string& type);
    void SetVolume(int volume);

private:
    Settings() = default;
    esp_err_t PersistLocked() const;

    mutable std::mutex mu_;
    SettingsData data_;
    bool loaded_ = false;
};

}  // namespace meet
