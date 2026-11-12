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
    // Every key, in file order.
    std::vector<std::string> keys() const;
    // Removes every key (dirty if there were any).
    void clear();
    // This file's key gets src's value (with its type); removed here when src has no such key.
    void copyKey(const SettingsFile& src, const std::string& key);
    std::string toText() const;
    // Writes the file when something changed since the last save. True if written or nothing to do.
    bool saveIfDirty();
    bool dirty() const;
    const std::wstring& path() const { return path_; }
    // Merging save (the shared setup, written by several games): saveIfDirty first reads the file again and
    // writes only the keys changed here since the last load or save on top of it, so two games running at the
    // same time keep each other's changes. The merged result is what this object holds afterwards.
    void setMerging(bool on) { merging_ = on; }
    bool merging() const { return merging_; }

private:
    struct Value {
        bool isString = true;
        std::string text;
        double number = 0;
    };
    const Value* lookup(const std::string& key) const;
    void touched(const std::string& key); // mutex_ held
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Value>> values_;
    std::vector<std::string> changed_; // keys set or removed since the last load/save (merging saves)
    bool cleared_ = false;             // clear() since the last load/save: keys not set again are dropped
    std::wstring path_;
    bool dirty_ = false;
    bool merging_ = false;
};

} // namespace horizon
