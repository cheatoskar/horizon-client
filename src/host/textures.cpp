// Pictures for the menu: the TM Horizon logo, plugin icons (plugin.json "icon"), other mods' icons.
// PNG decoded with stb_image, uploaded with createTextureRgba (managed pool: survives Reset).
#include "host/host.h"

#include <string>

#include "common/util.h"
#include "host/plugins.h"
#include "host/ui/ui.h"

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* data);

namespace horizon {
namespace {

uint64_t fromPng(const void* data, size_t size, int* w, int* h) {
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(static_cast<const unsigned char*>(data), static_cast<int>(size), w, h, &comp, 4);
    if (!px) return 0;
    const uint64_t tex = (*w > 0 && *h > 0 && *w <= 4096 && *h <= 4096) ? createTextureRgba(static_cast<uint32_t>(*w), static_cast<uint32_t>(*h), px) : 0;
    stbi_image_free(px);
    return tex;
}

uint64_t fromResource(int id, int* w, int* h) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fromResource), &self);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    return data ? fromPng(data, SizeofResource(self, r), w, h) : 0;
}

uint64_t fromFile(const std::wstring& path) {
    std::string bytes;
    if (!readFile(path, bytes) || bytes.empty()) return 0;
    int w = 0, h = 0;
    return fromPng(bytes.data(), bytes.size(), &w, &h);
}

struct Known {
    const char* name; // OtherMod::name
    int resource;
};
const Known kKnown[] = {{"TM Vibrant Shaders", 110}, {"CenterSpeed", 111}, {"Twinkie", 112}, {"TMInterface", 113},
                        {"TMUnlimiter", 114}, {"Competition Patch", 115}, {"KimMod", 116}, {"100% TMX + Bingo", 117}};

} // namespace

// After the overlay is ready on the device (game thread).
void texturesLoad() {
    int w = 0, h = 0;
    const uint64_t wordmark = fromResource(101, &w, &h);
    const float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    int mw = 0, mh = 0;
    const uint64_t mark = fromResource(102, &mw, &mh);
    ui::setLogo(wordmark, aspect, mark);
    int icons = 0;
    for (auto& up : plugins()) {
        const Plugin* p = up.get();
        if (p->manifest.icon.empty()) continue;
        if (const uint64_t t = fromFile(p->folder + wide(p->manifest.icon))) {
            ui::setIcon(p->id, t);
            icons++;
        } else {
            logLine(HORIZON_LOG_WARN, p->id.c_str(), "icon \"%s\" not loaded (a PNG in the plugin's folder)", p->manifest.icon.c_str());
        }
    }
    for (const auto& o : state().otherMods) {
        for (const auto& k : kKnown) {
            if (o.name != k.name) continue;
            int iw = 0, ih = 0;
            if (const uint64_t t = fromResource(k.resource, &iw, &ih)) ui::setIcon("other:" + o.name, t);
        }
    }
    HLOG("overlay: logo %s, %d plugin icon(s)", wordmark ? "loaded" : "NOT loaded", icons);
}

} // namespace horizon
