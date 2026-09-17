#include "widgets.hpp"
#include "imgcache.hpp"
#include "svgicons.hpp"
#include "util.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <unordered_map>

Fonts G;

// --------------------------------------------------------------------- theme

ImU32 theme::c(const char* hex) {
    // Hot path: cards call this dozens of times/frame — cache parsed colors.
    static std::unordered_map<std::string, ImU32> cache;
    auto it = cache.find(hex);
    if (it != cache.end()) return it->second;

    std::string s = hex;
    float alpha = 1.0f;
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        int a = std::stoi(s.substr(slash + 1), nullptr, 16);
        alpha = a / 255.0f;
        s = s.substr(0, slash);
    }
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    unsigned int v = (unsigned int)strtoul(s.c_str(), nullptr, 16);
    int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    ImU32 col = IM_COL32(r, g, b, (int)(alpha * 255));
    cache.emplace(hex, col);
    return col;
}

ImU32 theme::withA(ImU32 col, float alpha) {
    col &= 0x00FFFFFF;
    return col | ((ImU32)(alpha * 255) << 24);
}

void initFonts() {
    ImGuiIO& io = ImGui::GetIO();
    std::string base = std::string(APP_ASSET_DIR) + "/fonts/";
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    static ImVector<ImWchar> rangesPL;
    ImFontGlyphRangesBuilder rb;
    rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
    rb.AddText("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ…•–—→");
    rb.BuildRanges(&rangesPL);

    auto load = [&](const char* file, float px) -> ImFont* {
        std::string path = base + file;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, rangesPL.Data);
        if (!f) f = io.Fonts->AddFontDefault();
        return f;
    };
    G.r14 = load("Inter-Regular.ttf", 14);
    G.r16 = load("Inter-Regular.ttf", 16);
    G.r18 = load("Inter-Regular.ttf", 18);
    G.m16 = load("Inter-Medium.ttf", 16);
    G.m18 = load("Inter-Medium.ttf", 18);
    G.sb18 = load("Inter-SemiBold.ttf", 18);
    G.sb22 = load("Inter-SemiBold.ttf", 22);
    G.sb26 = load("Inter-SemiBold.ttf", 26);
    G.b28 = load("Inter-Bold.ttf", 28);
    G.b36 = load("Inter-Bold.ttf", 36);
}

// --------------------------------------------------------------------- icons

static void seg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t = 1.6f) {
    dl->AddLine(a, b, col, t);
}

namespace icons {

static bool svg(ImDrawList* dl, svgicon::Id id, ImVec2 ctr, float s, ImU32 col) {
    if (!svgicon::tex(id)) return false;
    svgicon::draw(dl, id, ctr, s, col);
    return true;
}

void search(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Search, ctr, s, col)) return;
    float r = s * 0.33f;
    ImVec2 c(ctr.x - s * 0.1f, ctr.y - s * 0.1f);
    dl->AddCircle(c, r, col, 16, 1.7f);
    seg(dl, ImVec2(c.x + r * 0.71f, c.y + r * 0.71f), ImVec2(ctr.x + s * 0.45f, ctr.y + s * 0.45f), col, 1.9f);
}

void star(ImDrawList* dl, ImVec2 ctr, float r, ImU32 col, bool filled) {
    if (svg(dl, svgicon::Star, ctr, r * 2.0f, col)) return;
    ImVec2 pts[10];
    for (int i = 0; i < 5; i++) {
        float aOut = -M_PI / 2 + i * 2 * M_PI / 5;
        float aIn = aOut + M_PI / 5;
        pts[2 * i] = ImVec2(ctr.x + cosf(aOut) * r, ctr.y + sinf(aOut) * r);
        pts[2 * i + 1] = ImVec2(ctr.x + cosf(aIn) * r * 0.42f, ctr.y + sinf(aIn) * r * 0.42f);
    }
    if (filled) dl->AddConvexPolyFilled(pts, 10, col);
    else for (int i = 0; i < 10; i++) dl->AddLine(pts[i], pts[(i + 1) % 10], col, 1.4f);
}

void download(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Download, ctr, s, col)) return;
    float x = ctr.x, y = ctr.y, h = s * 0.5f;
    seg(dl, ImVec2(x, y - h), ImVec2(x, y + h * 0.2f), col, 1.8f);
    seg(dl, ImVec2(x - s * 0.2f, y - h * 0.25f), ImVec2(x, y + h * 0.2f), col, 1.8f);
    seg(dl, ImVec2(x + s * 0.2f, y - h * 0.25f), ImVec2(x, y + h * 0.2f), col, 1.8f);
    dl->AddRect(ImVec2(x - h * 0.85f, y + h * 0.4f), ImVec2(x + h * 0.85f, y + h), col, 2.0f, 0, 1.6f);
}

void chevron(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, bool left) {
    if (svg(dl, left ? svgicon::ChevronLeft : svgicon::ChevronRight, ctr, s, col)) return;
    float k = s * 0.3f;
    if (!left) {
        seg(dl, ImVec2(ctr.x - k, ctr.y - s * 0.38f), ImVec2(ctr.x + k, ctr.y), col, 1.9f);
        seg(dl, ImVec2(ctr.x + k, ctr.y), ImVec2(ctr.x - k, ctr.y + s * 0.38f), col, 1.9f);
    } else {
        seg(dl, ImVec2(ctr.x + k, ctr.y - s * 0.38f), ImVec2(ctr.x - k, ctr.y), col, 1.9f);
        seg(dl, ImVec2(ctr.x - k, ctr.y), ImVec2(ctr.x + k, ctr.y + s * 0.38f), col, 1.9f);
    }
}

void play(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Play, ctr, s, col)) return;
    float k = s * 0.42f;
    ImVec2 pts[3] = { ImVec2(ctr.x - k * 0.6f, ctr.y - k), ImVec2(ctr.x - k * 0.6f, ctr.y + k), ImVec2(ctr.x + k, ctr.y) };
    dl->AddConvexPolyFilled(pts, 3, col);
}

void close(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, float thick) {
    if (svg(dl, svgicon::Close, ctr, s, col)) return;
    float k = s * 0.4f;
    seg(dl, ImVec2(ctr.x - k, ctr.y - k), ImVec2(ctr.x + k, ctr.y + k), col, thick);
    seg(dl, ImVec2(ctr.x + k, ctr.y - k), ImVec2(ctr.x - k, ctr.y + k), col, thick);
}

void sparkles(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Sparkles, ctr, s, col)) return;
    auto sparkle = [&](ImVec2 c, float r) {
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r * 0.28f, c.y), ImVec2(c.x + r * 0.28f, c.y), col);
        dl->AddTriangleFilled(ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.28f, c.y), ImVec2(c.x + r * 0.28f, c.y), col);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - r * 0.28f), ImVec2(c.x, c.y + r * 0.28f), col);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y), ImVec2(c.x, c.y - r * 0.28f), ImVec2(c.x, c.y + r * 0.28f), col);
    };
    sparkle(ImVec2(ctr.x - s * 0.12f, ctr.y + s * 0.1f), s * 0.33f);
    sparkle(ImVec2(ctr.x + s * 0.28f, ctr.y - s * 0.26f), s * 0.2f);
}

void film(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Film, ctr, s, col)) return;
    ImVec2 a(ctr.x - s * 0.45f, ctr.y - s * 0.36f), b(ctr.x + s * 0.45f, ctr.y + s * 0.36f);
    dl->AddRect(a, b, col, 2.5f, 0, 1.7f);
    float x1 = a.x + (b.x - a.x) / 3, x2 = a.x + 2 * (b.x - a.x) / 3;
    seg(dl, ImVec2(x1, a.y), ImVec2(x1, b.y), col, 1.4f);
    seg(dl, ImVec2(x2, a.y), ImVec2(x2, b.y), col, 1.4f);
}

void tv(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Tv, ctr, s, col)) return;
    ImVec2 a(ctr.x - s * 0.45f, ctr.y - s * 0.28f), b(ctr.x + s * 0.45f, ctr.y + s * 0.36f);
    dl->AddRect(a, b, col, 3.0f, 0, 1.7f);
    seg(dl, ImVec2(ctr.x - s * 0.14f, a.y - s * 0.26f), ImVec2(ctr.x - s * 0.34f, a.y), col, 1.7f);
    seg(dl, ImVec2(ctr.x + s * 0.14f, a.y - s * 0.26f), ImVec2(ctr.x + s * 0.34f, a.y), col, 1.7f);
}

void clock(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Clock, ctr, s, col)) return;
    float r = s * 0.4f;
    dl->AddCircle(ctr, r, col, 24, 1.7f);
    seg(dl, ctr, ImVec2(ctr.x, ctr.y - r * 0.55f), col, 1.7f);
    seg(dl, ctr, ImVec2(ctr.x + r * 0.45f, ctr.y + r * 0.1f), col, 1.7f);
}

void users(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Users, ctr, s, col)) return;
    dl->AddCircle(ImVec2(ctr.x - s * 0.14f, ctr.y - s * 0.18f), s * 0.16f, col, 12, 1.6f);
    dl->AddCircle(ImVec2(ctr.x + s * 0.28f, ctr.y - s * 0.12f), s * 0.12f, col, 12, 1.4f);
    dl->PathArcTo(ImVec2(ctr.x - s * 0.14f, ctr.y + s * 0.38f), s * 0.3f, M_PI, 2 * M_PI, 12);
    dl->PathStroke(col, 0, 1.6f);
    dl->PathArcTo(ImVec2(ctr.x + s * 0.4f, ctr.y + s * 0.38f), s * 0.22f, M_PI, 2 * M_PI, 12);
    dl->PathStroke(col, 0, 1.4f);
}

void cog(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Cog, ctr, s, col)) return;
    dl->AddCircle(ctr, s * 0.18f, col, 16, 1.6f);
    for (int i = 0; i < 8; i++) {
        float a = i * M_PI / 4;
        dl->AddLine(ImVec2(ctr.x + cosf(a) * s * 0.28f, ctr.y + sinf(a) * s * 0.28f),
                    ImVec2(ctr.x + cosf(a) * s * 0.44f, ctr.y + sinf(a) * s * 0.44f), col, 1.8f);
    }
}

void exclaim(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Exclaim, ctr, s, col)) return;
    float k = s * 0.45f;
    ImVec2 pts[3] = { ImVec2(ctr.x, ctr.y - k), ImVec2(ctr.x - k, ctr.y + k * 0.8f), ImVec2(ctr.x + k, ctr.y + k * 0.8f) };
    dl->AddPolyline(pts, 3, col, ImDrawFlags_Closed, 1.5f);
    seg(dl, ImVec2(ctr.x, ctr.y - k * 0.2f), ImVec2(ctr.x, ctr.y + k * 0.3f), col, 1.6f);
    dl->AddCircleFilled(ImVec2(ctr.x, ctr.y + k * 0.58f), 1.2f, col);
}

void eyeslash(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::EyeSlash, ctr, s, col)) return;
    dl->PathArcTo(ctr, s * 0.42f, M_PI + 0.6f, 2 * M_PI - 0.6f, 16);
    dl->PathStroke(col, 0, 1.5f);
    dl->PathArcTo(ImVec2(ctr.x, ctr.y + s * 0.1f), s * 0.42f, 0.6f, M_PI - 0.6f, 16);
    dl->PathStroke(col, 0, 1.5f);
    seg(dl, ImVec2(ctr.x - s * 0.42f, ctr.y - s * 0.32f), ImVec2(ctr.x + s * 0.42f, ctr.y + s * 0.32f), col, 1.7f);
}

void menu(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    for (int i = -1; i <= 1; i++)
        seg(dl, ImVec2(ctr.x - s * 0.42f, ctr.y + i * s * 0.28f), ImVec2(ctr.x + s * 0.42f, ctr.y + i * s * 0.28f), col, 1.9f);
}

void heart(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, bool filled) {
    if (svg(dl, svgicon::Heart, ctr, s, col)) return;
    float r = s * 0.21f;
    ImVec2 c1(ctr.x - r * 0.95f, ctr.y - s * 0.1f), c2(ctr.x + r * 0.95f, ctr.y - s * 0.1f);
    if (filled) {
        dl->AddCircleFilled(c1, r, col);
        dl->AddCircleFilled(c2, r, col);
        dl->AddTriangleFilled(ImVec2(c1.x - r * 0.85f, c1.y + r * 0.5f), ImVec2(c2.x + r * 0.85f, c2.y + r * 0.5f),
                              ImVec2(ctr.x, ctr.y + s * 0.45f), col);
    } else {
        dl->AddCircle(c1, r, col, 12, 1.5f);
        dl->AddCircle(c2, r, col, 12, 1.5f);
        seg(dl, ImVec2(c1.x - r * 0.7f, c1.y + r * 0.7f), ImVec2(ctr.x, ctr.y + s * 0.45f), col, 1.5f);
        seg(dl, ImVec2(c2.x + r * 0.7f, c2.y + r * 0.7f), ImVec2(ctr.x, ctr.y + s * 0.45f), col, 1.5f);
    }
}

void external(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::External, ctr, s, col)) return;
    float k = s * 0.36f;
    dl->AddLine(ImVec2(ctr.x - k * 0.2f, ctr.y + k * 0.2f), ImVec2(ctr.x + k, ctr.y - k), col, 1.6f);
    dl->AddLine(ImVec2(ctr.x + k * 0.35f, ctr.y - k), ImVec2(ctr.x + k, ctr.y - k), col, 1.6f);
    dl->AddLine(ImVec2(ctr.x + k, ctr.y - k), ImVec2(ctr.x + k, ctr.y - k * 0.35f), col, 1.6f);
    ImVec2 pts[6] = {
        ImVec2(ctr.x + k * 0.4f, ctr.y - k * 0.4f), ImVec2(ctr.x - k, ctr.y - k * 0.4f),
        ImVec2(ctr.x - k, ctr.y + k), ImVec2(ctr.x + k, ctr.y + k),
        ImVec2(ctr.x + k, ctr.y - k * 0.4f), ImVec2(ctr.x + k * 0.4f, ctr.y - k * 0.4f)
    };
    dl->AddPolyline(pts, 6, col, 0, 1.6f);
}

void refresh(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Refresh, ctr, s, col)) return;
    float r = s * 0.36f;
    dl->PathArcTo(ctr, r, -M_PI * 0.4f, M_PI * 0.9f, 20);
    dl->PathStroke(col, 0, 1.8f);
    ImVec2 tip(ctr.x + cosf(M_PI * 0.9f) * r, ctr.y + sinf(M_PI * 0.9f) * r);
    dl->AddTriangleFilled(ImVec2(tip.x - 1, tip.y - 4), ImVec2(tip.x + 5, tip.y + 1), ImVec2(tip.x - 4, tip.y + 3), col);
}

void plus(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Plus, ctr, s, col)) return;
    seg(dl, ImVec2(ctr.x - s * 0.4f, ctr.y), ImVec2(ctr.x + s * 0.4f, ctr.y), col, 2.0f);
    seg(dl, ImVec2(ctr.x, ctr.y - s * 0.4f), ImVec2(ctr.x, ctr.y + s * 0.4f), col, 2.0f);
}

void calendar(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Calendar, ctr, s, col)) return;
    ImVec2 a(ctr.x - s * 0.42f, ctr.y - s * 0.34f), b(ctr.x + s * 0.42f, ctr.y + s * 0.42f);
    dl->AddRect(a, b, col, 2.5f, 0, 1.6f);
    seg(dl, ImVec2(a.x, ctr.y - s * 0.08f), ImVec2(b.x, ctr.y - s * 0.08f), col, 1.6f);
    seg(dl, ImVec2(ctr.x - s * 0.2f, a.y - s * 0.12f), ImVec2(ctr.x - s * 0.2f, a.y + s * 0.06f), col, 1.6f);
    seg(dl, ImVec2(ctr.x + s * 0.2f, a.y - s * 0.12f), ImVec2(ctr.x + s * 0.2f, a.y + s * 0.06f), col, 1.6f);
}

void globe(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Globe, ctr, s, col)) return;
    float r = s * 0.4f;
    dl->AddCircle(ctr, r, col, 24, 1.6f);
    dl->AddEllipse(ctr, ImVec2(r * 0.5f, r), col, 0, 16, 1.3f);
    seg(dl, ImVec2(ctr.x - r, ctr.y), ImVec2(ctr.x + r, ctr.y), col, 1.3f);
}

void eye(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    if (svg(dl, svgicon::Eye, ctr, s, col)) return;
    dl->PathArcTo(ctr, s * 0.42f, M_PI + 0.6f, 2 * M_PI - 0.6f, 16);
    dl->PathStroke(col, 0, 1.5f);
    dl->PathArcTo(ImVec2(ctr.x, ctr.y + s * 0.08f), s * 0.42f, 0.6f, M_PI - 0.6f, 16);
    dl->PathStroke(col, 0, 1.5f);
    dl->AddCircleFilled(ctr, s * 0.1f, col);
}

// Matches ref spinner.svg: faint full ring + rotating arc (lightweight segments)
void spinner(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col) {
    float r = s * 0.42f;
    float thick = std::max(2.0f, s * 0.12f);
    dl->AddCircle(ctr, r, theme::withA(col, 0.35f), 16, thick);
    float t = (float)ImGui::GetTime();
    float a0 = t * 2.0f * (float)M_PI;
    dl->PathArcTo(ctr, r, a0, a0 + 1.65f, 12);
    dl->PathStroke(col, 0, thick);
}

} // namespace icons

// --------------------------------------------------------------------- text

ImVec2 w::textSize(const std::string& text, ImFont* font, float fontSize) {
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0, text.c_str());
}

// UTF-8 aware word wrap. Produces <= maxLines (+ellipsis) when maxLines>0.
static std::vector<std::string> wrapText(const std::string& text, ImFont* font, float fs, float maxW,
                                         int maxLines) {
    std::vector<std::string> out;
    if (!font || text.empty() || maxW <= 1.0f) return out;

    // split into words (UTF-8 safe: only split at spaces)
    std::vector<std::string> words;
    {
        std::string cur;
        for (size_t i = 0; i < text.size();) {
            unsigned char c = (unsigned char)text[i];
            size_t adv = 1;
            if (c >= 0xF0) adv = 4; else if (c >= 0xE0) adv = 3; else if (c >= 0xC0) adv = 2;
            if (i + adv > text.size()) adv = 1; // malformed trailing byte — don't overrun
            if (c == ' ' || c == '\n') {
                if (!cur.empty()) { words.push_back(std::move(cur)); cur.clear(); }
                if (c == '\n') words.push_back("\n");
                i += 1;
            } else {
                cur.append(text, i, adv);
                i += adv;
            }
        }
        if (!cur.empty()) words.push_back(std::move(cur));
    }

    std::string line;
    auto flushLine = [&]() { out.push_back(std::move(line)); line.clear(); };
    for (auto& wd : words) {
        if (wd == "\n") { if (!line.empty()) flushLine(); continue; }
        std::string cand = line.empty() ? wd : line + " " + wd;
        if (!line.empty() && font->CalcTextSizeA(fs, FLT_MAX, 0, cand.c_str()).x > maxW) {
            flushLine();
            line = std::move(wd);
        } else line = std::move(cand);
    }
    if (!line.empty()) flushLine();

    if (maxLines > 0 && (int)out.size() > maxLines) {
        out.resize((size_t)maxLines);
        if (out.empty()) return out;
        std::string& last = out.back();
        while (!last.empty() && font->CalcTextSizeA(fs, FLT_MAX, 0, (last + "…").c_str()).x > maxW) {
            size_t cut = last.size() - 1;
            while (cut > 0 && ((unsigned char)last[cut] & 0xC0) == 0x80) cut--;
            last.erase(cut);
        }
        last += "…";
    }
    return out;
}

float w::textClamped(ImDrawList* dl, ImVec2 pos, float maxW, const std::string& text,
                     ImFont* font, float fontSize, ImU32 col, int maxLines, float lineH) {
    if (text.empty()) return 0;
    if (lineH <= 0) lineH = fontSize * 1.28f;
    auto lines = wrapText(text, font, fontSize, maxW, maxLines);
    float y = 0;
    for (auto& ln : lines) {
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + y), col, ln.c_str());
        y += lineH;
    }
    return y;
}

// --------------------------------------------------------------------- images

void w::imageCover(ImDrawList* dl, GLuint tex, int imgW, int imgH, ImVec2 pos, ImVec2 size, ImU32 tint) {
    if (!tex || imgW <= 0 || imgH <= 0 || size.x <= 0 || size.y <= 0) return;
    ImVec2 min = pos, max(pos.x + size.x, pos.y + size.y);
    float scale = std::max(size.x / (float)imgW, size.y / (float)imgH);
    float u0 = 0.5f - size.x / (2 * imgW * scale);
    float v0 = 0.5f - size.y / (2 * imgH * scale);
    ImVec2 uv0(u0, v0), uv1(u0 + size.x / (imgW * scale), v0 + size.y / (imgH * scale));
    dl->AddImage((ImTextureID)(intptr_t)tex, min, max, uv0, uv1, tint);
}

void w::imageCoverRounded(ImDrawList* dl, GLuint tex, int imgW, int imgH, ImVec2 pos, ImVec2 size, float rounding, ImU32 tint) {
    if (!tex || imgW <= 0 || imgH <= 0 || size.x <= 0 || size.y <= 0) return;
    ImVec2 min = pos, max(pos.x + size.x, pos.y + size.y);
    float scale = std::max(size.x / (float)imgW, size.y / (float)imgH);
    float u0 = 0.5f - size.x / (2 * imgW * scale);
    float v0 = 0.5f - size.y / (2 * imgH * scale);
    ImVec2 uv0(u0, v0), uv1(u0 + size.x / (imgW * scale), v0 + size.y / (imgH * scale));
    float r = std::min(rounding, std::min(size.x, size.y) * 0.5f);
    dl->AddImageRounded((ImTextureID)(intptr_t)tex, min, max, uv0, uv1, tint, r);
}

void w::gradientRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 topCol, ImU32 botCol) {
    dl->AddRectFilledMultiColor(min, max, topCol, topCol, botCol, botCol);
}

// --------------------------------------------------------------------- small widgets

ImVec2 w::badge(ImDrawList* dl, ImVec2 pos, const std::string& text, ImU32 bg, ImU32 border, ImFont* font, float fs, ImU32 textCol) {
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
    ImVec2 sz(ts.x + 14, std::max(20.0f, ts.y + 6));
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg, sz.y / 2);
    if (border) dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), border, sz.y / 2, 0, 1.0f);
    dl->AddText(font, fs, ImVec2(pos.x + 7, pos.y + (sz.y - ts.y) / 2), textCol, text.c_str());
    return sz;
}

void w::voteCircle(ImDrawList* dl, ImVec2 pos, float r, double voteAvg) {
    double v = voteAvg;
    ImU32 col = v >= 6.5 ? theme::c("#10b981") : v >= 5.0 ? theme::c("#fbbf24") : theme::c("#ef4444");
    ImVec2 c(pos.x + r, pos.y + r);
    dl->AddCircle(c, r - 2, IM_COL32(255, 255, 255, 45), 32, 2.5f);
    if (v > 0) {
        dl->PathArcTo(c, r - 2, -(float)M_PI / 2, -(float)M_PI / 2 + (float)(v / 10.0) * 2 * (float)M_PI, 32);
        dl->PathStroke(col, 0, 2.5f);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), v > 0 ? "%.1f" : "?", v);
    std::string s = v > 0 ? std::string(buf) + (std::string(buf).find('.') != std::string::npos ? "" : ".0") : std::string(buf);
    // TMDB style: "7.4" or "10"
    ImVec2 ts = G.sb18->CalcTextSizeA(15, FLT_MAX, 0, s.c_str());
    dl->AddText(G.sb18, 15, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(255, 255, 255, 235), s.c_str());
}

bool w::button(ImDrawList* dl, const char* id, ImVec2 min, ImVec2 max, const std::string& label,
               ImU32 bg, ImU32 bgHover, ImU32 bgActive, ImU32 border, ImU32 textCol, ImFont* font, float fs,
               float rounding, float iconGap) {
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##btn", ImVec2(max.x - min.x, max.y - min.y));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    ImGui::PopID();
    ImU32 fill = held ? bgActive : hovered ? bgHover : bg;
    dl->AddRectFilled(min, max, fill, rounding);
    if (border) dl->AddRect(min, max, border, rounding, 0, 1.0f);
    if (!label.empty()) {
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
        ImVec2 tp((min.x + max.x) / 2 - ts.x / 2 + iconGap, (min.y + max.y) / 2 - ts.y / 2);
        dl->AddText(font, fs, tp, textCol, label.c_str());
    }
    return clicked;
}

bool w::iconButton(ImDrawList* dl, const char* id, ImVec2 min, ImVec2 max, void (*icon)(ImDrawList*, ImVec2, float, ImU32),
                   ImU32 bg, ImU32 bgHover, ImU32 border, ImU32 col, bool round) {
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##ib", ImVec2(max.x - min.x, max.y - min.y));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    float r = round ? (max.x - min.x) / 2 : 5;
    dl->AddRectFilled(min, max, hovered ? bgHover : bg, r);
    if (border) dl->AddRect(min, max, border, r, 0, 1.0f);
    ImVec2 ctr((min.x + max.x) / 2, (min.y + max.y) / 2);
    icon(dl, ctr, std::min(max.x - min.x, max.y - min.y) * 0.66f, col);
    return clicked;
}

// --------------------------------------------------------------------- title card
// Mirrors ref/seerr/src/components/TitleCard/index.tsx

int w::titleCard(const MediaItem& item, ImVec2 pos, float cw, float ch, bool watchlisted, int mediaStatus) {
    int result = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = pos, max(pos.x + cw, pos.y + ch);
    const float rad = 12;

    // Precomputed colors (theme::c is cached, but avoid even the map lookup storm)
    struct Cols {
        ImU32 bg, bgDark, pulse, ovTop, ovBot;
        ImU32 badgeTv, badgeTvBd, badgeMovie, badgeMovieBd;
        ImU32 stPending, stProc, stAvail;
        ImU32 ring, ringHov, starBg, starBgH, req, reqH, reqA, starOn;
        bool ready = false;
    };
    static Cols C;
    if (!C.ready) {
        C.bg = theme::c("#1f2937"); C.bgDark = theme::c("#111827"); C.pulse = theme::c("#374151");
        C.ovTop = theme::c("#2d3748/66"); C.ovBot = theme::c("#2d3748/E8");
        C.badgeTv = theme::c("#9333ea/C0"); C.badgeTvBd = theme::c("#a855f7");
        C.badgeMovie = theme::c("#2563eb/C0"); C.badgeMovieBd = theme::c("#3b82f6");
        C.stPending = theme::c("#3b82f6"); C.stProc = theme::c("#f59e0b"); C.stAvail = theme::c("#10b981");
        C.ring = theme::c("#374151"); C.ringHov = theme::c("#6b7280");
        C.starBg = theme::c("#111827/80"); C.starBgH = theme::c("#111827/CC");
        C.req = theme::c("#4f46e5"); C.reqH = theme::c("#6366f1"); C.reqA = theme::c("#4338ca");
        C.starOn = theme::c("#fcd34d");
        C.ready = true;
    }

    // By-value URL — avoid dangling refs; snapshot tex so map rehash can't UAF mid-draw
    std::string img = item.posterUrl("w300_and_h450_face");
    GLuint tex = 0; int tw = 0, th = 0;
    bool failed = false, loadingFlag = false;
    if (!img.empty()) {
        if (const ImageEntry* e = ImageCache::instance().request(img)) {
            tex = e->tex; tw = e->w; th = e->h;
            failed = e->failed; loadingFlag = e->loading;
        }
    }

    // Unique ID per on-screen instance (same title can appear in multiple Discover rows)
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID((int)item.mediaType);
    ImGui::PushID(item.id);
    ImGui::PushID((int)(pos.x * 10.0f) ^ ((int)(pos.y * 10.0f) << 16));
    bool clicked = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);
    float* hoverAnim = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("cardHov"), 0.0f);
    {
        float dt = ImGui::GetIO().DeltaTime;
        float target = hov ? 1.0f : 0.0f;
        *hoverAnim += (target - *hoverAnim) * (1.0f - expf(-14.0f * dt));
    }
    float ha = *hoverAnim;
    bool detail = ha > 0.02f;

    float reqBtnH = 28;
    float pad = 8;

    bool noPosterUrl = img.empty();
    bool hasPoster = tex != 0;
    bool giveUp = noPosterUrl || (failed && !loadingFlag);
    bool loading = !noPosterUrl && !hasPoster && !giveUp;

    if (hasPoster) {
        w::imageCoverRounded(dl, tex, tw, th, min, ImVec2(cw, ch), rad);
    } else if (loading) {
        float pulse = 0.45f + 0.35f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.2f));
        dl->AddRectFilled(min, max, theme::withA(C.pulse, pulse), rad);
        icons::spinner(dl, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.42f), 34,
                       IM_COL32(255, 255, 255, 200));
    } else {
        GLuint miss = ImageCache::instance().missingPosterTex;
        if (miss) w::imageCoverRounded(dl, miss, 300, 450, min, ImVec2(cw, ch), rad);
        else dl->AddRectFilled(min, max, C.bgDark, rad);
    }

    bool showOverlay = (detail || giveUp) && !loading;
    bool showReq = showOverlay && (mediaStatus == 0 || mediaStatus == 2) && ha > 0.15f;
    if (showOverlay) {
        auto scaleA = [](ImU32 c, float f) -> ImU32 {
            int a = (int)(((c >> 24) & 0xFF) * f);
            return (c & 0x00FFFFFF) | ((ImU32)a << 24);
        };
        float oa = (giveUp && !hov) ? 1.0f : ha;
        w::gradientRect(dl, min, max, scaleA(C.ovTop, oa), scaleA(C.ovBot, oa));
    }

    {
        const char* typeLabel = item.mediaType == MediaType::TV ? "SERIAL" : "FILM";
        ImU32 bg = item.mediaType == MediaType::TV ? C.badgeTv : C.badgeMovie;
        ImU32 bd = item.mediaType == MediaType::TV ? C.badgeTvBd : C.badgeMovieBd;
        ImFont* f = G.r14; float fs = 10;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, typeLabel);
        ImVec2 bp(min.x + 8, min.y + 8);
        ImVec2 bs(ts.x + 14, 20);
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), bg, 10);
        dl->AddRect(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), bd, 10, 0, 1.0f);
        dl->AddText(f, fs, ImVec2(bp.x + 7, bp.y + (bs.y - ts.y) / 2), IM_COL32(255, 255, 255, 255), typeLabel);
    }

    if (mediaStatus > 0) {
        ImU32 sc = mediaStatus == 1 ? C.stProc : mediaStatus == 2 ? theme::c("#ef4444") : C.stAvail;
        dl->AddCircleFilled(ImVec2(max.x - 15, min.y + 15), 5.5f, sc);
        dl->AddCircle(ImVec2(max.x - 15, min.y + 15), 5.5f, IM_COL32(0, 0, 0, 60), 12, 1.0f);
    }

    if (detail && !loading && ha > 0.2f) {
        ImVec2 sb(max.x - 36, min.y + 34);
        int sa = (int)(ha * 255);
        if (w::iconButton(dl, "##wl", sb, ImVec2(sb.x + 24, sb.y + 24),
                          [](ImDrawList* d, ImVec2 c, float s, ImU32 col) { icons::star(d, c, s * 0.5f, col, true); },
                          theme::withA(C.starBg, ha), theme::withA(C.starBgH, ha), 0,
                          watchlisted ? C.starOn : IM_COL32(255, 255, 255, sa), true))
            result = 3;
    }

    float textW = cw - pad * 2;
    float textClipBottom = max.y - pad - (showReq ? reqBtnH + 6 : 0);
    float textTopMin = min.y + 36;

    if (detail && !loading && ha > 0.12f) {
        int ta = (int)(ha * 255);
        std::string year = item.year();
        auto titleLines = wrapText(item.title, G.b28, 17, textW, 3);
        std::vector<std::string> sumLines;
        if (!item.overview.empty())
            sumLines = wrapText(item.overview, G.r14, 12, textW, 3);

        auto blockH = [&](int titleN, int sumN) {
            float h = (float)titleN * 20;
            if (!year.empty()) h += 16;
            if (sumN > 0) h += 4 + (float)sumN * 15;
            return h;
        };
        while (!sumLines.empty() &&
               textClipBottom - blockH((int)titleLines.size(), (int)sumLines.size()) < textTopMin)
            sumLines.pop_back();
        while (titleLines.size() > 1 &&
               textClipBottom - blockH((int)titleLines.size(), (int)sumLines.size()) < textTopMin)
            titleLines.pop_back();

        float ty = textClipBottom - blockH((int)titleLines.size(), (int)sumLines.size());
        if (ty < textTopMin) ty = textTopMin;
        // Soft rise on hover
        ty += (1.0f - ha) * 8.0f;

        if (textClipBottom > textTopMin + 2.0f) {
            dl->PushClipRect(ImVec2(min.x + 1, textTopMin - 2), ImVec2(max.x - 1, textClipBottom), true);
            if (!year.empty()) {
                dl->AddText(G.m16, 12.5f, ImVec2(min.x + pad, ty), IM_COL32(255, 255, 255, (int)(220 * ha)), year.c_str());
                ty += 16;
            }
            for (auto& ln : titleLines) {
                if (!ln.empty())
                    dl->AddText(G.b28, 17, ImVec2(min.x + pad, ty), IM_COL32(255, 255, 255, ta), ln.c_str());
                ty += 20;
            }
            if (!sumLines.empty()) {
                ty += 4;
                for (auto& ln : sumLines) {
                    if (!ln.empty())
                        dl->AddText(G.r14, 12, ImVec2(min.x + pad, ty), IM_COL32(255, 255, 255, (int)(190 * ha)), ln.c_str());
                    ty += 15;
                }
            }
            dl->PopClipRect();
        }
    } else if (giveUp && !loading) {
        float ty = max.y - pad - 22;
        w::textClamped(dl, ImVec2(min.x + pad, ty), textW, item.title, G.b28, 15, IM_COL32(255, 255, 255, 235), 1, 18);
    }

    dl->AddRect(min, max, hov ? C.ringHov : C.ring, rad, 0, hov ? 1.5f : 1.0f);

    if (clicked) result = 1;

    if (showReq) {
        float slide = (1.0f - ha) * 10.0f;
        ImVec2 bp(min.x + pad, max.y - pad - reqBtnH + slide);
        ImVec2 be(max.x - pad, max.y - pad + slide);
        int ba = (int)(ha * 255);
        dl->AddRectFilled(bp, be, theme::withA(C.req, ha), 6);
        ImGui::SetCursorScreenPos(bp);
        bool rb = ImGui::InvisibleButton("##req", ImVec2(be.x - bp.x, be.y - bp.y));
        bool rh = ImGui::IsItemHovered();
        bool ra = ImGui::IsItemActive();
        if (rh || ra) dl->AddRectFilled(bp, be, theme::withA(ra ? C.reqA : C.reqH, ha), 6);
        const char* lbl = "Zażądaj";
        ImVec2 ts = G.m16->CalcTextSizeA(13, FLT_MAX, 0, lbl);
        float cx = (bp.x + be.x) / 2, total = ts.x + 18;
        icons::download(dl, ImVec2(cx - total / 2 + 6, (bp.y + be.y) / 2), 13, IM_COL32(255, 255, 255, ba));
        dl->AddText(G.m16, 13, ImVec2(cx - total / 2 + 16, (bp.y + be.y) / 2 - ts.y / 2), IM_COL32(255, 255, 255, ba), lbl);
        if (rb) result = 2;
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    return result;
}

bool w::castCard(const CastMember& m, ImVec2 pos, float size) {
    std::string url = m.profilePath.empty() ? "" : Tmdb::img("w185", m.profilePath);
    const ImageEntry* e = url.empty() ? nullptr : ImageCache::instance().request(url);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = pos, max(pos.x + size, pos.y + size);
    float rad = size / 2;
    dl->AddRectFilled(min, max, theme::c("#1f2937"), rad);
    bool has = e && e->tex;
    bool loading = !url.empty() && !has && !(e && e->failed && !e->loading);
    if (has) {
        w::imageCoverRounded(dl, e->tex, e->w, e->h, min, ImVec2(size, size), rad);
    } else if (loading) {
        icons::spinner(dl, ImVec2(min.x + size / 2, min.y + size / 2), size * 0.35f, IM_COL32(255, 255, 255, 180));
    } else {
        dl->AddCircleFilled(ImVec2(min.x + size / 2, min.y + size * 0.38f), size * 0.17f, theme::c("#374151"));
        dl->PathArcTo(ImVec2(min.x + size / 2, max.y + size * 0.14f), size * 0.42f, (float)M_PI, 2 * (float)M_PI, 20);
        dl->PathStroke(theme::c("#374151"), 0, size * 0.3f);
    }

    float ty = max.y + 6;
    w::textClamped(dl, ImVec2(pos.x - 12, ty), size + 24, m.name, G.m16, 13, IM_COL32(255, 255, 255, 235), 2);
    w::textClamped(dl, ImVec2(pos.x - 12, ty + 2 * 16), size + 24, m.character, G.r14, 12, theme::c("#9ca3af"), 2);

    ImGui::SetCursorScreenPos(min);
    return ImGui::InvisibleButton("##cast", ImVec2(size, size + 62));
}
