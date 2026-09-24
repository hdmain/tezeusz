#pragma once
#include <string>

struct GLFWwindow;

// System tray: close-to-tray, Show / Quit from the icon menu.
namespace tray {

bool init(GLFWwindow* win, const std::string& iconPngPath);
void shutdown();

// Pump tray events (call every frame from the UI thread).
void tick();

void hideToTray();
void showFromTray();
bool isHidden();

// Latched once; caller clears by acting on them.
bool consumeShowRequest();
bool consumeQuitRequest();

} // namespace tray
