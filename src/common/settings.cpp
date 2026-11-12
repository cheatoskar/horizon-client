#include "common/settings.h"

#include "common/json.h"
#include "common/util.h"

#include <algorithm>
#include <cstdlib>

namespace horizon {

namespace {
// The JSON object's flat values (nested values are not settings). False if text is not a JSON object.
template <typename V>
bool parseFlat(const std::string& text, std::vector<std::pair<std::string, V>>& out) {
    Json root;
    std::string err;
    out.clear();
    if (!JsonParser::parse(text, root, err) || root.type != Json::Object) return false;
    for (const auto& kv : root.obj) {
        V v;
        switch (kv.second.type) {
        case Json::String: v.text = kv.second.str; break;
        case Json::Number: v.isString = false; v.number = kv.second.num; break;
        case Json::Bool: v.isString = false; v.number = kv.second.b ? 1 : 0; break;
        default: continue;
        }
        out.emplace_back(kv.first, v);
    }
    return true;
}
} // namespace

bool SettingsFile::load(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        values_.clear();
        changed_.clear();
        cleared_ = false;
        dirty_ = false;
    }
    std::string text;
    if (!readFile(path, text)) return true; // missing: empty
    return loadText(text);
}

bool SettingsFile::loadText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    changed_.clear();
    cleared_ = false;
    return parseFlat(text, values_);
}

const SettingsFile::Value* SettingsFile::lookup(const std::string& key) const {
    for (const auto& kv : values_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void SettingsFile::touched(const std::string& key) {
    dirty_ = true;
    if (std::find(changed_.begin(), changed_.end(), key) == changed_.end()) changed_.push_back(key);
}

bool SettingsFile::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(key) != nullptr;
}

std::string SettingsFile::getString(const std::string& key, const std::string& fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Value* v = lookup(key);
    if (!v) return fallback;
    if (v->isString) return v->text;
    char buf[64];
    if (v->number == static_cast<double>(static_cast<long long>(v->number))) snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v->number));
    else snprintf(buf, sizeof(buf), "%.9g", v->number);
    return buf;
}

int SettingsFile::getInt(const std::string& key, int fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Value* v = lookup(key);
    if (!v) return fallback;
    if (!v->isString) return static_cast<int>(v->number);
    char* end = nullptr;
    const long r = strtol(v->text.c_str(), &end, 0);
    return end && end != v->text.c_str() ? static_cast<int>(r) : fallback;
}

float SettingsFile::getFloat(const std::string& key, float fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Value* v = lookup(key);
    if (!v) return fallback;
    if (!v->isString) return static_cast<float>(v->number);
    char* end = nullptr;
    const double r = strtod(v->text.c_str(), &end);
    return end && end != v->text.c_str() ? static_cast<float>(r) : fallback;
}

void SettingsFile::setString(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : values_) {
        if (kv.first == key) {
            if (kv.second.isString && kv.second.text == value) return;
            kv.second = Value{true, value, 0};
            touched(key);
            return;
        }
    }
    values_.emplace_back(key, Value{true, value, 0});
    touched(key);
}

void SettingsFile::setNumber(const std::string& key, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : values_) {
        if (kv.first == key) {
            if (!kv.second.isString && kv.second.number == value) return;
            kv.second = Value{false, std::string(), value};
            touched(key);
            return;
        }
    }
    values_.emplace_back(key, Value{false, std::string(), value});
    touched(key);
}

void SettingsFile::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < values_.size(); i++) {
        if (values_[i].first == key) {
            values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(i));
            touched(key);
            return;
        }
    }
}

std::vector<std::string> SettingsFile::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& kv : values_) out.push_back(kv.first);
    return out;
}

void SettingsFile::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    changed_.clear();
    cleared_ = true;
    dirty_ = true; // written even when it was empty here: the file on disk may still hold keys
}

void SettingsFile::copyKey(const SettingsFile& src, const std::string& key) {
    if (&src == this) return;
    bool found = false;
    Value v;
    {
        std::lock_guard<std::mutex> lock(src.mutex_);
        if (const Value* s = src.lookup(key)) {
            v = *s;
            found = true;
        }
    }
    if (!found) {
        remove(key);
        return;
    }
    if (v.isString) setString(key, v.text);
    else setNumber(key, v.number);
}

std::string SettingsFile::toText() const {
    Json root;
    root.type = Json::Object;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : values_) {
            Json v;
            if (kv.second.isString) {
                v.type = Json::String;
                v.str = kv.second.text;
            } else {
                v.type = Json::Number;
                v.num = kv.second.number;
            }
            root.obj.emplace_back(kv.first, v);
        }
    }
    std::string out;
    jsonWrite(out, root);
    out += "\n";
    return out;
}

bool SettingsFile::dirty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirty_;
}

bool SettingsFile::saveIfDirty() {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dirty_ || path_.empty()) return true;
        path = path_;
    }
    if (merging_) {
        // what is on disk now (another game may have written it), with this object's changes on top
        std::vector<std::pair<std::string, Value>> disk;
        std::string text;
        if (readFile(path, text)) parseFlat(text, disk);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cleared_) disk.clear();
            for (const std::string& key : changed_) {
                const Value* mine = lookup(key);
                auto it = std::find_if(disk.begin(), disk.end(), [&](const std::pair<std::string, Value>& kv) { return kv.first == key; });
                if (mine && it != disk.end()) it->second = *mine;
                else if (mine) disk.emplace_back(key, *mine);
                else if (it != disk.end()) disk.erase(it);
            }
            values_ = disk;
        }
    }
    const std::string text = toText();
    ensureDir(folderOf(path));
    if (!writeFileAtomic(path, text)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = false;
    changed_.clear();
    cleared_ = false;
    return true;
}

} // namespace horizon
