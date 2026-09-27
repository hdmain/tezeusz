#pragma once
#include "imgui.h"

// Soft backdrop for question/confirm modals: captures the last sharp frame and
// draws a downscaled Gaussian-ish blur instead of ImGui's default light dim.
namespace modalblur {

// Call after ImGui_ImplOpenGL3_RenderDrawData, before SwapBuffers.
void onFrameEnd(int fbW, int fbH, bool anyModalOpen);

// Draw blurred scrim into an ImGui draw list (fullscreen rect a..b).
void draw(ImDrawList* dl, ImVec2 a, ImVec2 b);

void shutdown();

} // namespace modalblur
