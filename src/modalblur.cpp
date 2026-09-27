#include "modalblur.hpp"
#include "gl_compat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace modalblur {
namespace {

GLuint g_tex = 0;
int g_texW = 0;
int g_texH = 0;
bool g_have = false;
bool g_wasModal = false;
int g_captureCountdown = 0;

std::vector<uint8_t> g_raw;   // last sharp capture (fb size, RGBA, bottom-up)
std::vector<uint8_t> g_blur;  // downscaled blurred RGB(A)
int g_rawW = 0, g_rawH = 0;

void ensureTex(int w, int h) {
    if (g_tex && g_texW == w && g_texH == h) return;
    if (g_tex) {
        glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g_texW = w;
    g_texH = h;
}

// Separable box blur (3 passes ≈ soft Gaussian) on RGBA buffer.
void boxBlurPass(std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int w, int h, int radius, bool horizontal) {
    dst.resize(src.size());
    const int diam = radius * 2 + 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int k = -radius; k <= radius; k++) {
                int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                const uint8_t* p = src.data() + ((size_t)sy * w + sx) * 4;
                r += p[0]; g += p[1]; b += p[2]; a += p[3];
            }
            uint8_t* o = dst.data() + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)(r / diam);
            o[1] = (uint8_t)(g / diam);
            o[2] = (uint8_t)(b / diam);
            o[3] = (uint8_t)(a / diam);
        }
    }
}

void buildBlurTexture() {
    if (g_rawW < 2 || g_rawH < 2 || g_raw.empty() || !glTexImage2D) return;

    // Downscale ~1/4 for speed, then blur.
    const int sw = std::max(2, g_rawW / 4);
    const int sh = std::max(2, g_rawH / 4);
    std::vector<uint8_t> small((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++) {
        const int sy0 = y * g_rawH / sh;
        for (int x = 0; x < sw; x++) {
            const int sx0 = x * g_rawW / sw;
            const uint8_t* s = g_raw.data() + ((size_t)sy0 * g_rawW + sx0) * 4;
            uint8_t* d = small.data() + ((size_t)y * sw + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }

    std::vector<uint8_t> tmp;
    const int radius = 3;
    for (int pass = 0; pass < 3; pass++) {
        boxBlurPass(small, tmp, sw, sh, radius, true);
        boxBlurPass(tmp, small, sw, sh, radius, false);
    }

    // Flip vertically for ImGui UV (GL read is bottom-up).
    g_blur.resize(small.size());
    for (int y = 0; y < sh; y++) {
        std::memcpy(g_blur.data() + ((size_t)y * sw) * 4,
                    small.data() + ((size_t)(sh - 1 - y) * sw) * 4,
                    (size_t)sw * 4);
    }

    ensureTex(sw, sh);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_blur.data());
    g_have = true;
}

void captureRaw(int fbW, int fbH) {
    if (!glReadPixels || fbW < 2 || fbH < 2) return;
    g_rawW = fbW;
    g_rawH = fbH;
    g_raw.resize((size_t)fbW * fbH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, g_raw.data());
}

} // namespace

void onFrameEnd(int fbW, int fbH, bool anyModalOpen) {
    if (!glReadPixels) return;

    if (!anyModalOpen) {
        // Keep a recent sharp snapshot while the UI is idle (avoid every-frame hitch).
        if (--g_captureCountdown <= 0) {
            captureRaw(fbW, fbH);
            g_captureCountdown = 12;
        }
        g_wasModal = false;
        return;
    }

    // First frame the modal appears: build blur from the last sharp capture.
    if (!g_wasModal) {
        if (g_raw.empty())
            captureRaw(fbW, fbH);
        buildBlurTexture();
    }
    g_wasModal = true;
}

void draw(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    if (g_have && g_tex) {
        ImTextureID id = (ImTextureID)(intptr_t)g_tex;
        dl->AddImage(id, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
        // Slight cool tint so it reads as frosted glass, not a photo.
        dl->AddRectFilled(a, b, IM_COL32(8, 10, 18, 120));
    } else {
        dl->AddRectFilled(a, b, IM_COL32(8, 10, 18, 200));
    }
}

void shutdown() {
    if (g_tex) {
        glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    g_have = false;
    g_raw.clear();
    g_blur.clear();
    g_texW = g_texH = g_rawW = g_rawH = 0;
}

} // namespace modalblur
