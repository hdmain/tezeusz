#pragma once
#include "gl_compat.hpp"
#include "imgui.h"
#include "tmdb.hpp"
#include <string>

namespace theme {
ImU32 c(const char* hex);            // "#111827" -> ImU32, with optional /80 alpha suffix e.g. "#4f46e5/80"
ImU32 withA(ImU32 col, float alpha); // alpha 0..1
}

struct Fonts {
    ImFont* r14 = nullptr; ImFont* r16 = nullptr; ImFont* r18 = nullptr;
    ImFont* m16 = nullptr; ImFont* m18 = nullptr;
    ImFont* sb18 = nullptr; ImFont* sb22 = nullptr; ImFont* sb26 = nullptr;
    ImFont* b28 = nullptr; ImFont* b36 = nullptr;
};
extern Fonts G;
void initFonts();

namespace icons {
void search(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void star(ImDrawList* dl, ImVec2 ctr, float r, ImU32 col, bool filled);
void download(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);      // arrow-down-tray
void chevron(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, bool left);
void play(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void close(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, float thick);
void sparkles(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void film(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void tv(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void clock(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void users(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void cog(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void exclaim(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void eyeslash(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void menu(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void heart(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col, bool filled);
void external(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void refresh(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void plus(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void calendar(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void globe(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
void eye(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
// Animated spinner (matches ref/seerr spinner.svg)
void spinner(ImDrawList* dl, ImVec2 ctr, float s, ImU32 col);
}

namespace w {

// Indigo orbiting-dots spinner (used on quit / player loading)
void orbitSpinner(ImDrawList* dl, ImVec2 ctr, float radius = 14.f, float dotR = 3.2f);

// returns drawn height; wraps text, clamps to maxLines with ellipsis
float textClamped(ImDrawList* dl, ImVec2 pos, float maxW, const std::string& text,
                  ImFont* font, float fontSize, ImU32 col, int maxLines, float lineH = 0);
ImVec2 textSize(const std::string& text, ImFont* font, float fontSize);

void imageCover(ImDrawList* dl, GLuint tex, int imgW, int imgH, ImVec2 pos, ImVec2 size, ImU32 tint = IM_COL32(255,255,255,255));
void imageCoverRounded(ImDrawList* dl, GLuint tex, int imgW, int imgH, ImVec2 pos, ImVec2 size, float rounding, ImU32 tint = IM_COL32(255,255,255,255));
void gradientRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 topCol, ImU32 botCol, float rounding = 0.0f);

// pill badge; returns size used
ImVec2 badge(ImDrawList* dl, ImVec2 pos, const std::string& text, ImU32 bg, ImU32 border, ImFont* font, float fs, ImU32 textCol);

// TMDB-style vote circle
void voteCircle(ImDrawList* dl, ImVec2 pos, float r, double voteAvg);

// rounded button. Returns true when clicked. alignRight: draws right-aligned ending at max.x
bool button(ImDrawList* dl, const char* id, ImVec2 min, ImVec2 max, const std::string& label,
            ImU32 bg, ImU32 bgHover, ImU32 bgActive, ImU32 border, ImU32 textCol, ImFont* font, float fs,
            float rounding = 12.0f, float iconGap = 0);

// icon-only square button (returns clicked)
bool iconButton(ImDrawList* dl, const char* id, ImVec2 min, ImVec2 max, void (*icon)(ImDrawList*, ImVec2, float, ImU32),
                ImU32 bg, ImU32 bgHover, ImU32 border, ImU32 col, bool round = false);

// full title card as in Seerr (w x h poster, hover overlay, request button).
// mediaStatus: 0 unknown, 1 pending, 2 processing, 3 available (for badge colors)
// Returns: 0 none, 1 card clicked (open details), 2 request clicked, 3 watchlist toggled
int titleCard(const MediaItem& item, ImVec2 pos, float cw, float ch, bool watchlisted, int mediaStatus,
              int instanceSeed = 0, bool allowHover = true);

// circular cast card; returns clicked
bool castCard(const CastMember& m, ImVec2 pos, float size);
}
