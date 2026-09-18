#pragma once
#include <string>

struct GLFWwindow;

namespace platform {
void openUrl(const std::string& url);
void openPath(const std::string& path);

// Native Win11-style dark titlebar (keeps OS caption; no custom chrome).
void applyDarkTitlebar(GLFWwindow* win);
}
