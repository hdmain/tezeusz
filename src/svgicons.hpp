#pragma once
#include "gl_compat.hpp"
#include "imgui.h"
#include <string>

// Heroicon SVG → GL textures (white glyphs, tint via ImGui col).
namespace svgicon {
enum Id {
    Sparkles, Film, Tv, Clock, EyeSlash, Exclaim, Users, Cog,
    Search, Close, Star, Download, ChevronLeft, ChevronRight, Play,
    Heart, Refresh, Plus, Calendar, Globe, Eye, External,
    COUNT
};

void init();   // call once after GL context
void shutdown();
GLuint tex(Id id); // 0 if missing
int sizePx();      // raster size (e.g. 64)

// Draw centered icon of logical size `s` (screen px) with tint `col`.
void draw(ImDrawList* dl, Id id, ImVec2 ctr, float s, ImU32 col);
}
