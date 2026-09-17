#include "svgicons.hpp"
#include "util.hpp"

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg.h>
#include <nanosvgrast.h>

namespace svgicon {
namespace {

constexpr int kSize = 64;
GLuint g_tex[COUNT] = {};

const char* fileFor(Id id) {
    switch (id) {
    case Sparkles: return "sparkles.svg";
    case Film: return "film.svg";
    case Tv: return "tv.svg";
    case Clock: return "clock.svg";
    case EyeSlash: return "eye-slash.svg";
    case Exclaim: return "exclamation-triangle.svg";
    case Users: return "users.svg";
    case Cog: return "cog.svg";
    case Search: return "magnifying-glass.svg";
    case Close: return "x-mark.svg";
    case Star: return "star.svg";
    case Download: return "arrow-down-tray.svg";
    case ChevronLeft: return "chevron-left.svg";
    case ChevronRight: return "chevron-right.svg";
    case Play: return "play.svg";
    case Heart: return "heart.svg";
    case Refresh: return "arrow-path.svg";
    case Plus: return "plus.svg";
    case Calendar: return "calendar.svg";
    case Globe: return "globe-alt.svg";
    case Eye: return "eye.svg";
    case External: return "arrow-top-right-on-square.svg";
    default: return nullptr;
    }
}

GLuint uploadRgba(const uint8_t* rgba, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void replaceAll(std::string& s, const char* from, const char* to) {
    size_t n = std::strlen(from), m = std::strlen(to);
    for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; ) {
        s.replace(i, n, to);
        i += m;
    }
}

GLuint rasterizeSvg(const std::string& path) {
    std::string svg = util::readFile(path);
    if (svg.empty()) return 0;
    // Heroicons use currentColor — NanoSVG needs a real paint.
    replaceAll(svg, "currentColor", "#ffffff");
    replaceAll(svg, "currentcolor", "#ffffff");

    NSVGimage* image = nsvgParse(svg.data(), "px", 96.0f);
    if (!image || image->width <= 0 || image->height <= 0) {
        if (image) nsvgDelete(image);
        return 0;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return 0;
    }

    std::vector<uint8_t> rgba((size_t)kSize * kSize * 4, 0);
    float scale = (float)kSize / std::max(image->width, image->height);
    float dx = ((float)kSize - image->width * scale) * 0.5f;
    float dy = ((float)kSize - image->height * scale) * 0.5f;
    nsvgRasterize(rast, image, dx, dy, scale, rgba.data(), kSize, kSize, kSize * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // Boost near-transparent anti-aliased edge alpha a touch for crisp UI icons.
    for (int i = 0; i < kSize * kSize; i++) {
        uint8_t& a = rgba[(size_t)i * 4 + 3];
        if (a > 0 && a < 40) a = (uint8_t)std::min(255, (int)a * 2);
        // Force RGB white so ImGui tint works cleanly.
        if (a) {
            rgba[(size_t)i * 4 + 0] = 255;
            rgba[(size_t)i * 4 + 1] = 255;
            rgba[(size_t)i * 4 + 2] = 255;
        }
    }
    return uploadRgba(rgba.data(), kSize, kSize);
}

} // namespace

void init() {
    const std::string dir = std::string(APP_ASSET_DIR) + "/icons/";
    for (int i = 0; i < COUNT; i++) {
        const char* f = fileFor((Id)i);
        if (!f) continue;
        g_tex[i] = rasterizeSvg(dir + f);
    }
}

void shutdown() {
    for (int i = 0; i < COUNT; i++) {
        if (g_tex[i]) {
            GLuint t = g_tex[i];
            glDeleteTextures(1, &t);
            g_tex[i] = 0;
        }
    }
}

GLuint tex(Id id) {
    if (id < 0 || id >= COUNT) return 0;
    return g_tex[id];
}

int sizePx() { return kSize; }

void draw(ImDrawList* dl, Id id, ImVec2 ctr, float s, ImU32 col) {
    GLuint t = tex(id);
    if (!t || !dl) return;
    float half = s * 0.5f;
    ImVec2 a(ctr.x - half, ctr.y - half), b(ctr.x + half, ctr.y + half);
    // Slight padding so stroke icons don't clip.
    const float pad = s * 0.04f;
    a.x += pad; a.y += pad; b.x -= pad; b.y -= pad;
    dl->AddImage((ImTextureID)(intptr_t)t, a, b, ImVec2(0, 0), ImVec2(1, 1), col);
}

} // namespace svgicon
