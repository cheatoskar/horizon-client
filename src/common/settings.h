// A flat key/value settings file (JSON object), one per owner: horizon\settings\<id>.json.
// Values keep their JSON type (string / number / bool); getters convert. Thread-safe.
#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace horizon {

class SettingsFile {
public:
    // Loads path (missing file = empty). False if the file exists but is not a JSON object (then it
    // stays empty and is not overwritten until something is set).
    bool load(const std::wstring& path);
    bool loadText(const std::string& text);
    std::string getString(const std::string& key, const std::string& fallback) const;
    int getInt(const std::string& key, int fallback) const;
    float getFloat(const std::string& key, float fallback) const;
    bool has(const std::string& key) const;
    void setString(const std::string& key, const std::string& value);
    void setNumber(const std::string& key, double value);
    void remove(const std::string& key);
    std::string toText() const;
    // Writes the file when something changed since the last save. True if written or nothing to do.
    bool saveIfDirty();
    bool dirty() const;
    const std::wstring& path() const { return path_; }

private:
    struct Value {
        bool isString = true;
        std::string text;
        double number = 0;
    };
    const Value* lookup(const std::string& key) const;
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Value>> values_;
    std::wstring path_;
    bool dirty_ = false;
};

} // namespace horizon
