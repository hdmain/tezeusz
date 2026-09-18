#include "platform.hpp"
#include <cstdlib>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#endif

namespace platform {

void openUrl(const std::string& url) {
    if (url.empty()) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

void openPath(const std::string& path) {
    if (path.empty()) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

void applyDarkTitlebar(GLFWwindow* win) {
    if (!win) return;
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) return;

    // Keep the default Windows caption/buttons — just switch to the modern dark theme.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Win11 colored caption (falls back gracefully on older builds)
    COLORREF border = RGB(17, 24, 39);   // #111827
    COLORREF caption = RGB(31, 41, 55);  // #1f2937
    COLORREF text = RGB(229, 231, 235);  // #e5e7eb
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
#else
    (void)win;
#endif
}

} // namespace platform
