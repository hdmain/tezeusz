#include "updater.hpp"
#include "http.hpp"
#include "util.hpp"
#include "core.hpp"
#include "player.hpp"
#include "i18n.hpp"
#include "sha256.hpp"
#include "stack.hpp"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <spawn.h>
#include <fcntl.h>
extern char** environ;
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace updater {
namespace {

#ifndef SEERR_VERSION
#define SEERR_VERSION "0.1.0-dev"
#endif
#ifndef SEERR_UPDATE_REPO
#define SEERR_UPDATE_REPO "hdmain/tezeusz"
#endif
#ifndef SEERR_UPDATE_CHANNEL
#define SEERR_UPDATE_CHANNEL "continuous"
#endif

std::atomic<State> g_state{State::Idle};
std::mutex g_mu;
std::string g_status;
std::string g_remoteVer;
int g_dlPercent = 0;
std::atomic<bool> g_wantQuit{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_checkRequested{false};
std::atomic<bool> g_workerBusy{false};
double g_readySince = 0;
std::string g_stageDir;
std::string g_installDir;

std::string currentVersion() { return SEERR_VERSION; }

std::string platformId() {
#ifdef _WIN32
    return "windows-x64";
#else
    return "linux-x64";
#endif
}

std::string manifestUrl() {
    return std::string("https://github.com/") + SEERR_UPDATE_REPO +
           "/releases/download/" + SEERR_UPDATE_CHANNEL +
           "/update-manifest-" + platformId() + ".json";
}

std::string blobUrl(const std::string& baseUrl, const std::string& sha) {
    std::string b = baseUrl;
    if (!b.empty() && b.back() != '/') b.push_back('/');
    return b + "b_" + sha;
}

void setStatus(State st, const std::string& text) {
    g_state.store(st);
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = text;
}

bool dirWritable(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    auto probe = fs::path(dir) / ".seerr-write-test";
    {
        std::ofstream f(probe.string(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "ok";
    }
    fs::remove(probe, ec);
    return true;
}

std::string normalizeRel(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    while (!p.empty() && p[0] == '/') p.erase(p.begin());
    return p;
}

struct RemoteFile {
    std::string path;
    std::string sha256;
    int64_t size = 0;
};

struct Manifest {
    std::string version;
    std::string platform;
    std::string baseUrl;
    std::vector<RemoteFile> files;
};

bool parseManifest(const std::string& body, Manifest* out, std::string* err) {
    try {
        auto j = json::parse(body);
        Manifest m;
        m.version = j.value("version", "");
        m.platform = j.value("platform", "");
        m.baseUrl = j.value("baseUrl", "");
        if (m.baseUrl.empty()) {
            m.baseUrl = std::string("https://github.com/") + SEERR_UPDATE_REPO +
                        "/releases/download/" + SEERR_UPDATE_CHANNEL + "/";
        }
        if (!j.contains("files") || !j["files"].is_array()) {
            if (err) *err = "manifest missing files[]";
            return false;
        }
        for (auto& f : j["files"]) {
            RemoteFile rf;
            rf.path = normalizeRel(f.value("path", ""));
            rf.sha256 = util::lower(f.value("sha256", ""));
            rf.size = f.value("size", (int64_t)0);
            if (rf.path.empty() || rf.sha256.size() != 64) continue;
            // path traversal guard
            if (rf.path.find("..") != std::string::npos) continue;
            m.files.push_back(std::move(rf));
        }
        if (m.files.empty()) {
            if (err) *err = "manifest has no usable files";
            return false;
        }
        *out = std::move(m);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool writeFileAtomic(const std::string& path, const std::vector<uint8_t>& data) {
    fs::path p(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

#ifdef _WIN32
bool spawnApplyScript(const std::string& script, const std::string& pid,
                      const std::string& stage, const std::string& dest) {
    // ShellExecuteEx avoids broken cmd.exe quoting with paths that end in '\'.
    std::string params = pid + " \"" + stage + "\" \"" + dest + "\"";
    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = "open";
    sei.lpFile = script.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

bool writeApplyScript(const std::string& path) {
    // NOTE: never write "%DEST%\" — the trailing \" eats the closing quote on Windows.
    const char* body =
        "@echo off\r\n"
        "setlocal EnableExtensions\r\n"
        "set \"PID=%~1\"\r\n"
        "set \"STAGE=%~2\"\r\n"
        "set \"DEST=%~3\"\r\n"
        "set \"LOG=%TEMP%\\seerr-update-apply.log\"\r\n"
        "echo apply start %DATE% %TIME% PID=%PID% > \"%LOG%\"\r\n"
        "echo STAGE=%STAGE%>> \"%LOG%\"\r\n"
        "echo DEST=%DEST%>> \"%LOG%\"\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq %PID%\" 2>nul | find \"%PID%\" >nul\r\n"
        "if not errorlevel 1 (\r\n"
        "  timeout /t 1 /nobreak >nul\r\n"
        "  goto wait\r\n"
        ")\r\n"
        "timeout /t 2 /nobreak >nul\r\n"
        "if not exist \"%STAGE%\" (\r\n"
        "  echo missing STAGE>> \"%LOG%\"\r\n"
        "  exit /b 1\r\n"
        ")\r\n"
        "robocopy \"%STAGE%\" \"%DEST%\" /E /IS /IT /R:3 /W:1 /NFL /NDL /NJH /NJS /NC /NS >> \"%LOG%\" 2>&1\r\n"
        "if errorlevel 8 (\r\n"
        "  echo copy failed - keeping STAGE for retry>> \"%LOG%\"\r\n"
        "  exit /b 1\r\n"
        ")\r\n"
        "if not exist \"%DEST%\\seerr.exe\" (\r\n"
        "  echo missing seerr.exe after copy>> \"%LOG%\"\r\n"
        "  exit /b 1\r\n"
        ")\r\n"
        "rmdir /S /Q \"%STAGE%\" 2>nul\r\n"
        "echo ok, restarting>> \"%LOG%\"\r\n"
        "start \"\" /D \"%DEST%\" \"%DEST%\\seerr.exe\"\r\n"
        "del \"%~f0\" 2>nul\r\n"
        "exit /b 0\r\n";
    return util::writeFile(path, body);
}
#else
bool spawnApplyScript(const std::string& script, const std::string& pid,
                      const std::string& stage, const std::string& dest) {
    std::string bash = "/bin/bash";
    std::string scriptCopy = script, pidCopy = pid, stageCopy = stage, destCopy = dest;
    char* argv[] = {
        bash.data(), scriptCopy.data(), pidCopy.data(), stageCopy.data(), destCopy.data(), nullptr
    };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    int rc = posix_spawn(&child, bash.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0;
}

bool writeApplyScript(const std::string& path) {
    const char* body =
        "#!/bin/bash\n"
        "PID=\"$1\"; STAGE=\"$2\"; DEST=\"$3\"\n"
        "LOG=\"${TMPDIR:-/tmp}/seerr-update-apply.log\"\n"
        "echo \"apply start $(date) PID=$PID\" > \"$LOG\"\n"
        "while kill -0 \"$PID\" 2>/dev/null; do sleep 0.4; done\n"
        "sleep 0.8\n"
        "if [ ! -d \"$STAGE\" ]; then echo missing STAGE >> \"$LOG\"; exit 1; fi\n"
        "if ! cp -a \"$STAGE\"/. \"$DEST\"/; then echo cp failed >> \"$LOG\"; exit 1; fi\n"
        "chmod +x \"$DEST/seerr\" 2>/dev/null || true\n"
        "if [ ! -x \"$DEST/seerr\" ]; then echo missing seerr >> \"$LOG\"; exit 1; fi\n"
        "rm -rf \"$STAGE\"\n"
        "echo ok, restarting >> \"$LOG\"\n"
        "(cd \"$DEST\" && nohup ./seerr >/dev/null 2>&1 &)\n"
        "rm -f \"$0\"\n"
        "exit 0\n";
    if (!util::writeFile(path, body)) return false;
    chmod(path.c_str(), 0755);
    return true;
}
#endif

void beginApply() {
    if (g_stageDir.empty() || g_installDir.empty()) return;
    setStatus(State::Applying, i18n::tr("update.applying"));
#ifdef _WIN32
    std::string script = (fs::path(util::appDataPath("update")) / "apply-update.bat").string();
    std::string pid = std::to_string(GetCurrentProcessId());
#else
    std::string script = (fs::path(util::appDataPath("update")) / "apply-update.sh").string();
    std::string pid = std::to_string(getpid());
#endif
    fs::create_directories(fs::path(script).parent_path());
    if (!writeApplyScript(script) || !spawnApplyScript(script, pid, g_stageDir, g_installDir)) {
        setStatus(State::Error, i18n::tr("update.apply_failed"));
        return;
    }
    g_wantQuit.store(true);
}

void workerCheckAndDownload() {
    if (g_stop.load()) return;
    setStatus(State::Checking, i18n::tr("update.checking"));
    g_dlPercent = 0;

    std::string err;
    auto resp = http::get(manifestUrl(), "application/json");
    if (!resp.ok()) {
        setStatus(State::Error, i18n::tr("update.check_failed"));
        return;
    }
    Manifest man;
    if (!parseManifest(resp.body, &man, &err)) {
        setStatus(State::Error, i18n::tr("update.bad_manifest"));
        return;
    }
    if (man.platform != platformId()) {
        setStatus(State::Error, i18n::tr("update.bad_platform"));
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_remoteVer = man.version;
    }

    if (!man.version.empty() && man.version == currentVersion()) {
        setStatus(State::UpToDate, i18n::tr("update.up_to_date"));
        return;
    }

    // Diff against local install dir
    std::vector<RemoteFile> need;
    for (auto& f : man.files) {
        fs::path local = fs::path(g_installDir) / f.path;
        std::error_code ec;
        if (!fs::exists(local, ec)) {
            need.push_back(f);
            continue;
        }
        std::string localSha = sha256::ofFile(local.string());
        if (localSha != f.sha256)
            need.push_back(f);
    }

    if (need.empty()) {
        // Same files, version string differs (rebuild) — treat as up to date
        setStatus(State::UpToDate, i18n::tr("update.up_to_date"));
        return;
    }

    setStatus(State::Downloading,
              std::string(i18n::tr("update.downloading")) + " (" +
                  std::to_string(need.size()) + ")");

    // Keep any already-valid staged files (resume after a failed apply).
    std::error_code ec;
    fs::create_directories(g_stageDir, ec);

    int done = 0;
    for (auto& f : need) {
        if (g_stop.load()) return;
        fs::path dest = fs::path(g_stageDir) / f.path;
        {
            std::error_code ec2;
            if (fs::exists(dest, ec2)) {
                std::string staged = sha256::ofFile(dest.string());
                if (staged == f.sha256) {
                    ++done;
                    g_dlPercent = (int)((done * 100) / (int)need.size());
                    {
                        std::lock_guard<std::mutex> lk(g_mu);
                        g_status = std::string(i18n::tr("update.downloading")) + " " +
                                   std::to_string(done) + "/" + std::to_string(need.size());
                    }
                    continue;
                }
                fs::remove(dest, ec2);
            }
        }

        std::string url = blobUrl(man.baseUrl, f.sha256);
        std::string dlErr;
        auto bytes = http::getBinaryLong(url, &dlErr, 180);
        if (bytes.empty()) {
            setStatus(State::Error, i18n::tr("update.download_failed") + std::string(": ") + f.path +
                                        (dlErr.empty() ? "" : (" (" + dlErr + ")")));
            return;
        }
        std::string got = sha256::ofBytes(bytes.data(), bytes.size());
        if (got != f.sha256) {
            setStatus(State::Error, i18n::tr("update.checksum_failed") + std::string(": ") + f.path);
            return;
        }
        if (!writeFileAtomic(dest.string(), bytes)) {
            setStatus(State::Error, i18n::tr("update.write_failed"));
            return;
        }
        ++done;
        g_dlPercent = (int)((done * 100) / (int)need.size());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status = std::string(i18n::tr("update.downloading")) + " " +
                       std::to_string(done) + "/" + std::to_string(need.size());
        }
    }

    // Also write a marker with target version
    util::writeFile((fs::path(g_stageDir) / ".seerr-update-version").string(), man.version);
    g_dlPercent = 100;
    g_readySince = 0;
    setStatus(State::Ready, i18n::tr("update.ready"));
}

void enqueueCheck() {
    if (g_workerBusy.exchange(true)) return;
    core::enqueue([]() {
        workerCheckAndDownload();
        g_workerBusy.store(false);
    });
}

} // anon

void init() {
    g_stop.store(false);
    g_wantQuit.store(false);
    g_checkRequested.store(false);
    g_installDir = util::exeDir();
    g_stageDir = util::appDataPath("update-stage");

    if (const char* dis = std::getenv("SEERR_DISABLE_UPDATE"); dis && dis[0] && dis[0] != '0') {
        setStatus(State::Disabled, i18n::tr("update.disabled"));
        return;
    }
    if (!stack::StackConfig::get().autoUpdate) {
        setStatus(State::Disabled, i18n::tr("update.disabled_user"));
        return;
    }
    if (!dirWritable(g_installDir)) {
        setStatus(State::Disabled, i18n::tr("update.not_writable"));
        return;
    }
    setStatus(State::Idle, i18n::tr("update.idle"));
    // First check shortly after start (background).
    g_checkRequested.store(true);
}

void shutdown() {
    g_stop.store(true);
}

void checkNow() {
    if (const char* dis = std::getenv("SEERR_DISABLE_UPDATE"); dis && dis[0] && dis[0] != '0') {
        setStatus(State::Disabled, i18n::tr("update.disabled"));
        return;
    }
    if (!dirWritable(g_installDir)) {
        setStatus(State::Disabled, i18n::tr("update.not_writable"));
        return;
    }
    // Manual check is allowed even when auto-update is off.
    State st = g_state.load();
    if (st == State::Disabled)
        setStatus(State::Idle, i18n::tr("update.checking"));
    g_checkRequested.store(true);
}

void setAutoEnabled(bool on) {
    auto& cfg = stack::StackConfig::get();
    cfg.autoUpdate = on;
    cfg.save();
    if (!on) {
        g_checkRequested.store(false);
        g_wantQuit.store(false);
        g_readySince = 0;
        State st = g_state.load();
        if (st != State::Downloading && st != State::Checking && st != State::Applying)
            setStatus(State::Disabled, i18n::tr("update.disabled_user"));
        return;
    }
    if (const char* dis = std::getenv("SEERR_DISABLE_UPDATE"); dis && dis[0] && dis[0] != '0') {
        setStatus(State::Disabled, i18n::tr("update.disabled"));
        return;
    }
    if (!dirWritable(g_installDir)) {
        setStatus(State::Disabled, i18n::tr("update.not_writable"));
        return;
    }
    setStatus(State::Idle, i18n::tr("update.idle"));
    g_checkRequested.store(true);
}

bool autoEnabled() {
    return stack::StackConfig::get().autoUpdate;
}

void tick() {
    State st = g_state.load();
    if (st == State::Applying) return;

    const bool autoOn = stack::StackConfig::get().autoUpdate;

    if (g_checkRequested.exchange(false)) {
        if (st != State::Downloading && st != State::Checking && st != State::Ready && st != State::Applying)
            enqueueCheck();
    }

    st = g_state.load();
    if (st == State::Disabled) return;

    // Periodic re-check + auto-apply only when auto-update is enabled.
    if (!autoOn) return;

    static double lastPeriodic = 0;
    double now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count() /
                 1000.0;
    if ((st == State::Idle || st == State::UpToDate || st == State::Error) &&
        !g_workerBusy.load()) {
        if (lastPeriodic <= 0) lastPeriodic = now;
        else if (now - lastPeriodic > 6 * 3600.0) {
            lastPeriodic = now;
            enqueueCheck();
        }
    }

    if (st == State::Ready && !player::isOpen()) {
        if (g_readySince <= 0) g_readySince = now;
        if (now - g_readySince >= 1.5)
            beginApply();
    }
}

State state() { return g_state.load(); }

std::string statusText() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

std::string remoteVersion() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_remoteVer;
}

int downloadPercent() { return g_dlPercent; }

bool wantsQuitForApply() { return g_wantQuit.load(); }

} // namespace updater
