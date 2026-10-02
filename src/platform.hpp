#pragma once
#include <string>

struct GLFWwindow;

namespace platform {
void openUrl(const std::string& url);
void openPath(const std::string& path);

// Native open-file dialog. filterPairs: "Label\0*.ext;*.ext2\0" (Win) / ignored hint on Linux.
// Returns empty string if cancelled. If err is set and no dialog backend is available, err explains why.
std::string pickOpenFile(const char* title, const char* filterLabel, const char* filterPattern,
                         std::string* err = nullptr);

// Native Win11-style dark titlebar (keeps OS caption; no custom chrome).
void applyDarkTitlebar(GLFWwindow* win);

// Keep display awake while media is playing (screensaver / idle sleep).
// Safe to call every frame - only toggles when `active` changes.
void setIdleInhibit(bool active, const char* reason = "Playing video");
}
