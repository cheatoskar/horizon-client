// Thumbnail decoding with a private static copy of stb_image (JPEG, PNG, BMP): the shared ckcommon.lib build is PNG only.
#include "tmx_logic.h"

#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_TGA
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include "stb_image.h"

namespace tmx {

bool decodeThumb(const void* data, size_t len, std::vector<uint8_t>& rgba, int& w, int& h, int maxWidth) {
    if (!data || len < 8 || len > (16u << 20) || maxWidth < 1) return false;
    int sw = 0, sh = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(static_cast<const unsigned char*>(data), static_cast<int>(len), &sw, &sh, &comp, 4);
    if (!px) return false;
    if (sw < 1 || sh < 1 || sw > 8192 || sh > 8192) {
        stbi_image_free(px);
        return false;
    }
    int dw = sw, dh = sh;
    if (sw > maxWidth) {
        dw = maxWidth;
        dh = sh * maxWidth / sw;
        if (dh < 1) dh = 1;
    }
    rgba.assign(static_cast<size_t>(dw) * dh * 4, 0);
    if (dw == sw && dh == sh) {
        memcpy(rgba.data(), px, rgba.size());
    } else { // box filter
        for (int y = 0; y < dh; y++) {
            const int y0 = static_cast<int>(static_cast<long long>(y) * sh / dh);
            int y1 = static_cast<int>(static_cast<long long>(y + 1) * sh / dh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                const int x0 = static_cast<int>(static_cast<long long>(x) * sw / dw);
                int x1 = static_cast<int>(static_cast<long long>(x + 1) * sw / dw);
                if (x1 <= x0) x1 = x0 + 1;
                unsigned sum[4] = {0, 0, 0, 0};
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        const unsigned char* p = px + (static_cast<size_t>(yy) * sw + xx) * 4;
                        for (int c = 0; c < 4; c++) sum[c] += p[c];
                    }
                const unsigned n = static_cast<unsigned>((y1 - y0) * (x1 - x0));
                uint8_t* o = &rgba[(static_cast<size_t>(y) * dw + x) * 4];
                for (int c = 0; c < 4; c++) o[c] = static_cast<uint8_t>(sum[c] / n);
            }
        }
    }
    stbi_image_free(px);
    w = dw;
    h = dh;
    return true;
}

} // namespace tmx
