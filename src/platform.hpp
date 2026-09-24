#pragma once
#include <string>

struct GLFWwindow;

namespace platform {
void openUrl(const std::string& url);
void openPath(const std::string& path);

// Native Win11-style dark titlebar (keeps OS caption; no custom chrome).
void applyDarkTitlebar(GLFWwindow* win);

// Keep display awake while media is playing (screensaver / idle sleep).
// Safe to call every frame — only toggles when `active` changes.
void setIdleInhibit(bool active, const char* reason = "Playing video");
}
