#include "settings_meta.h"

#include <ctype.h>
#include <stdlib.h>

namespace hs {
namespace {

bool parseNumber(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    *out = strtod(s.c_str(), &end);
    return end && *end == 0;
}

} // namespace

bool parseSettingMeta(const std::string& meta, SettingMeta* out, std::string* error) {
    *out = SettingMeta();
    error->clear();
    size_t p = 0;
    auto skip = [&] {
        while (p < meta.size() && isspace(static_cast<unsigned char>(meta[p]))) p++;
    };
    skip();
    std::string word;
    while (p < meta.size() && (isalnum(static_cast<unsigned char>(meta[p])) || meta[p] == '_')) word += meta[p++];
    if (word != "Setting") return false;
    for (;;) {
        skip();
        if (p >= meta.size()) break;
        std::string key;
        while (p < meta.size() && (isalnum(static_cast<unsigned char>(meta[p])) || meta[p] == '_')) key += meta[p++];
        if (key.empty()) {
            *error = "unexpected '" + std::string(1, meta[p]) + "' in [Setting ...]";
            return true;
        }
        skip();
        std::string value;
        bool hasValue = false;
        if (p < meta.size() && meta[p] == '=') {
            p++;
            skip();
            hasValue = true;
            if (p < meta.size() && meta[p] == '"') {
                p++;
                while (p < meta.size() && meta[p] != '"') {
                    if (meta[p] == '\\' && p + 1 < meta.size()) p++;
                    value += meta[p++];
                }
                if (p >= meta.size()) {
                    *error = "text after " + key + "= is not closed with \"";
                    return true;
                }
                p++;
            } else {
                while (p < meta.size() && !isspace(static_cast<unsigned char>(meta[p]))) value += meta[p++];
            }
        }
        if (key == "name") out->name = value;
        else if (key == "description") out->description = value;
        else if (key == "hidden") out->hidden = !hasValue || value == "true";
        else if (key == "multiline") out->multiline = !hasValue || value == "true";
        else if (key == "color") out->color = !hasValue || value == "true";
        else if (key == "min" || key == "max") {
            double d = 0;
            if (!parseNumber(value, &d)) {
                *error = key + " must be a number";
                continue;
            }
            if (key == "min") {
                out->min = d;
                out->hasMin = true;
            } else {
                out->max = d;
                out->hasMax = true;
            }
        } else if (key == "options") {
            size_t s = 0;
            while (s <= value.size()) {
                size_t e = value.find('|', s);
                if (e == std::string::npos) e = value.size();
                out->options.push_back(value.substr(s, e - s));
                s = e + 1;
            }
        } else {
            *error = "unknown setting attribute \"" + key + "\"";
        }
    }
    if (out->hasMin && out->hasMax && out->min > out->max) {
        *error = "min is larger than max";
        out->hasMin = out->hasMax = false;
    }
    return true;
}

} // namespace hs
