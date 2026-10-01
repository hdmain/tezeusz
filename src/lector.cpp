#include "lector.hpp"
#include "core.hpp"
#include "http.hpp"
#include "i18n.hpp"
#include "stack.hpp"
#include "util.hpp"
#include "zipwrite.hpp"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lector {
namespace {

constexpr const char* kHfBase =
    "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/";
constexpr const char* kPiperBase =
    "https://github.com/rhasspy/piper/releases/download/2023.11.14-2/";

std::mutex g_mu;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_dlBusy{false};
std::atomic<bool> g_jobBusy{false};
std::atomic<float> g_progress{0.f};
std::atomic<float> g_jobProgress{0.f};
std::atomic<uint64_t> g_dlGot{0};
std::atomic<uint64_t> g_dlTotal{0};
std::atomic<double> g_dlSpeedBps{0};
State g_state = State::Disabled;
std::string g_status;
std::string g_statusDetail;
std::string g_error;
std::string g_jobMsg;
std::string g_jobErr;
std::string g_jobOut;
uint64_t g_dlGen = 0;
uint64_t g_jobGen = 0;
struct PendingGen {
    std::string video;
    std::string srt;
    std::string voice;
};
PendingGen g_pendingGen;
bool g_hasPendingGen = false;

const std::vector<VoiceInfo>& voiceCatalog() {
    static const std::vector<VoiceInfo> k = {
        {"pl_PL-darkman-medium", "pl", "pl_PL", "Darkman", "medium",
         "pl/pl_PL/darkman/medium"},
        {"pl_PL-gosia-medium", "pl", "pl_PL", "Gosia", "medium",
         "pl/pl_PL/gosia/medium"},
        {"en_US-lessac-medium", "en", "en_US", "Lessac", "medium",
         "en/en_US/lessac/medium"},
        {"en_US-amy-medium", "en", "en_US", "Amy", "medium",
         "en/en_US/amy/medium"},
        {"en_GB-alan-medium", "en", "en_GB", "Alan", "medium",
         "en/en_GB/alan/medium"},
        {"de_DE-thorsten-medium", "de", "de_DE", "Thorsten", "medium",
         "de/de_DE/thorsten/medium"},
        {"fr_FR-siwis-medium", "fr", "fr_FR", "Siwis", "medium",
         "fr/fr_FR/siwis/medium"},
        {"es_ES-sharvard-medium", "es", "es_ES", "Sharvard", "medium",
         "es/es_ES/sharvard/medium"},
        {"it_IT-paola-medium", "it", "it_IT", "Paola", "medium",
         "it/it_IT/paola/medium"},
        {"uk_UA-ukrainian_tts-medium", "uk", "uk_UA", "Ukrainian TTS", "medium",
         "uk/uk_UA/ukrainian_tts/medium"},
        {"ru_RU-denis-medium", "ru", "ru_RU", "Denis", "medium",
         "ru/ru_RU/denis/medium"},
        {"cs_CZ-jirka-medium", "cs", "cs_CZ", "Jirka", "medium",
         "cs/cs_CZ/jirka/medium"},
    };
    return k;
}

const VoiceInfo* findVoice(const std::string& id) {
    for (auto& v : voiceCatalog())
        if (v.id == id) return &v;
    return nullptr;
}

fs::path rootDir() { return fs::path(util::appDataPath("lector")); }
fs::path engineDir() { return rootDir() / "piper"; }
fs::path voicesDir() { return rootDir() / "voices"; }
fs::path samplesDir() { return rootDir() / "samples"; }
fs::path workDir() { return rootDir() / "work"; }

#ifdef _WIN32
fs::path piperExe() { return engineDir() / "piper" / "piper.exe"; }
#else
fs::path piperExe() { return engineDir() / "piper" / "piper"; }
#endif

void setStatus(State st, const std::string& msg, const std::string& err = {}) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_state = st;
    g_status = msg;
    if (!err.empty()) g_error = err;
}

void setStatusDetail(const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_statusDetail = detail;
}

void clearError() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_error.clear();
}

std::string formatBytes(uint64_t n) {
    char buf[64];
    if (n >= 1024ull * 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.2f GB", n / (1024.0 * 1024.0 * 1024.0));
    else if (n >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    else if (n >= 1024ull)
        std::snprintf(buf, sizeof(buf), "%.0f KB", n / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    return buf;
}

std::string formatEta(double seconds) {
    if (!(seconds > 0) || seconds > 24 * 3600) return "--:--";
    int s = (int)(seconds + 0.5);
    int m = s / 60;
    s %= 60;
    int h = m / 60;
    m %= 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

struct TransferMeter {
    std::string label;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastT = t0;
    uint64_t lastBytes = 0;
    double emaBps = 0;

    void begin(const std::string& lab) {
        label = lab;
        t0 = lastT = std::chrono::steady_clock::now();
        lastBytes = 0;
        emaBps = 0;
        g_dlGot = 0;
        g_dlTotal = 0;
        g_dlSpeedBps = 0;
        g_progress = 0.f;
        setStatus(State::Downloading, lab);
        setStatusDetail({});
    }

    void onProgress(uint64_t got, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        const double dt =
            std::chrono::duration<double>(now - lastT).count();
        if (dt >= 0.15 || got < lastBytes || (total > 0 && got >= total)) {
            const double inst =
                dt > 0.001 ? (double)((got > lastBytes) ? (got - lastBytes) : 0) / dt : 0.0;
            if (emaBps <= 1.0) emaBps = inst;
            else emaBps = emaBps * 0.72 + inst * 0.28;
            lastT = now;
            lastBytes = got;
        }
        g_dlGot = got;
        g_dlTotal = total;
        g_dlSpeedBps = emaBps;
        if (total > 0)
            g_progress = std::min(1.f, (float)got / (float)total);
        else
            g_progress = 0.f;

        std::string detail;
        if (total > 0)
            detail = formatBytes(got) + " / " + formatBytes(total);
        else
            detail = formatBytes(got);
        if (emaBps > 500)
            detail += "  ·  " + formatBytes((uint64_t)emaBps) + "/s";
        if (total > got && emaBps > 500) {
            detail += "  ·  ETA " + formatEta((double)(total - got) / emaBps);
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status = label;
            g_statusDetail = detail;
            g_state = State::Downloading;
        }
    }
};

bool writeBytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return (bool)out;
}

std::string samplePhrase(const std::string& lang) {
    if (lang == "pl") return "Witaj. To jest próbka lektora AI w Seerr.";
    if (lang == "de") return "Hallo. Dies ist eine Probe des KI-Sprechers.";
    if (lang == "fr") return "Bonjour. Ceci est un échantillon du narrateur IA.";
    if (lang == "es") return "Hola. Esta es una muestra del narrador de IA.";
    if (lang == "it") return "Ciao. Questo è un campione del narratore IA.";
    if (lang == "uk") return "Вітаю. Це зразок AI-лектора.";
    if (lang == "ru") return "Здравствуйте. Это образец ИИ-лектора.";
    if (lang == "cs") return "Ahoj. Toto je ukázka AI lektora.";
    return "Hello. This is a sample of the AI narrator in Seerr.";
}

// ---- WAV helpers ----
struct WavData {
    int sampleRate = 22050;
    int channels = 1;
    std::vector<int16_t> pcm;
};

bool readWav(const fs::path& path, WavData& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char riff[12];
    in.read(riff, 12);
    if (in.gcount() != 12 || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0)
        return false;
    int16_t audioFormat = 0, channels = 0, bits = 0;
    int32_t sampleRate = 0;
    std::vector<char> dataChunk;
    while (in) {
        char id[4];
        uint32_t sz = 0;
        in.read(id, 4);
        in.read(reinterpret_cast<char*>(&sz), 4);
        if (!in) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<char> fmt(sz);
            in.read(fmt.data(), sz);
            if (sz >= 16) {
                std::memcpy(&audioFormat, fmt.data(), 2);
                std::memcpy(&channels, fmt.data() + 2, 2);
                std::memcpy(&sampleRate, fmt.data() + 4, 4);
                std::memcpy(&bits, fmt.data() + 14, 2);
            }
            if (sz & 1) in.seekg(1, std::ios::cur);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataChunk.resize(sz);
            in.read(dataChunk.data(), sz);
            if (sz & 1) in.seekg(1, std::ios::cur);
        } else {
            in.seekg(sz + (sz & 1), std::ios::cur);
        }
    }
    if (audioFormat != 1 || bits != 16 || channels < 1 || sampleRate <= 0 || dataChunk.empty())
        return false;
    out.sampleRate = sampleRate;
    out.channels = channels;
    size_t n = dataChunk.size() / 2;
    out.pcm.resize(n);
    std::memcpy(out.pcm.data(), dataChunk.data(), n * 2);
    return true;
}

bool writeWav(const fs::path& path, const WavData& w) {
    if (w.pcm.empty() || w.sampleRate <= 0 || w.channels <= 0) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const uint32_t dataBytes = (uint32_t)(w.pcm.size() * sizeof(int16_t));
    const uint16_t channels = (uint16_t)w.channels;
    const uint32_t rate = (uint32_t)w.sampleRate;
    const uint16_t bits = 16;
    const uint32_t byteRate = rate * channels * (bits / 8);
    const uint16_t blockAlign = (uint16_t)(channels * (bits / 8));
    const uint32_t fmtSize = 16;
    const uint32_t riffSize = 36 + dataBytes;
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    uint16_t format = 1;
    out.write(reinterpret_cast<const char*>(&format), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&rate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(w.pcm.data()), dataBytes);
    return (bool)out;
}

// ---- SRT ----
struct Cue {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string text;
};

int64_t parseTs(const std::string& s) {
    // 00:01:02,345 or 00:01:02.345
    int h = 0, m = 0, sec = 0, ms = 0;
    char sep = ',';
    if (std::sscanf(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4 ||
        std::sscanf(s.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4)
        return ((int64_t)h * 3600 + (int64_t)m * 60 + sec) * 1000 + ms;
    (void)sep;
    return 0;
}

std::string stripTags(std::string t) {
    std::string out;
    out.reserve(t.size());
    bool inTag = false;
    for (char c : t) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out.push_back(c);
    }
    for (char& c : out)
        if (c == '\n' || c == '\r') c = ' ';
    return util::trim(out);
}

std::vector<Cue> parseSrt(const std::string& path) {
    std::string raw = util::readFile(path);
    if (raw.empty()) return {};
    // Drop UTF-8 BOM
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        raw.erase(0, 3);
    std::vector<Cue> cues;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (util::trim(line).empty()) continue;
        // index line (optional)
        std::string ts;
        if (line.find("-->") == std::string::npos) {
            if (!std::getline(ss, ts)) break;
            if (!ts.empty() && ts.back() == '\r') ts.pop_back();
        } else {
            ts = line;
        }
        auto arrow = ts.find("-->");
        if (arrow == std::string::npos) continue;
        std::string a = util::trim(ts.substr(0, arrow));
        std::string b = util::trim(ts.substr(arrow + 3));
        // strip position hints after timestamp
        auto sp = b.find(' ');
        if (sp != std::string::npos) b = b.substr(0, sp);
        Cue c;
        c.startMs = parseTs(a);
        c.endMs = parseTs(b);
        std::string text;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (util::trim(line).empty()) break;
            if (!text.empty()) text.push_back('\n');
            text += line;
        }
        c.text = stripTags(text);
        if (!c.text.empty() && c.endMs > c.startMs) cues.push_back(std::move(c));
    }
    return cues;
}

// ---- process ----
unsigned piperThreadCount(const std::string& speed) {
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    if (speed == "hard") return cores;
    if (speed == "medium") return std::max(1u, cores / 2);
    return 0; // normal: leave ORT default (as before)
}

void applyPiperLoadEnv(const std::string& speed) {
    const unsigned n = piperThreadCount(speed);
#ifdef _WIN32
    if (n == 0) {
        SetEnvironmentVariableA("OMP_NUM_THREADS", nullptr);
        SetEnvironmentVariableA("OPENBLAS_NUM_THREADS", nullptr);
        SetEnvironmentVariableA("MKL_NUM_THREADS", nullptr);
        SetEnvironmentVariableA("OMP_WAIT_POLICY", nullptr);
        return;
    }
    const std::string ns = std::to_string(n);
    SetEnvironmentVariableA("OMP_NUM_THREADS", ns.c_str());
    SetEnvironmentVariableA("OPENBLAS_NUM_THREADS", ns.c_str());
    SetEnvironmentVariableA("MKL_NUM_THREADS", ns.c_str());
    SetEnvironmentVariableA("OMP_WAIT_POLICY", speed == "hard" ? "ACTIVE" : "PASSIVE");
#else
    if (n == 0) {
        unsetenv("OMP_NUM_THREADS");
        unsetenv("OPENBLAS_NUM_THREADS");
        unsetenv("MKL_NUM_THREADS");
        unsetenv("OMP_WAIT_POLICY");
        return;
    }
    const std::string ns = std::to_string(n);
    setenv("OMP_NUM_THREADS", ns.c_str(), 1);
    setenv("OPENBLAS_NUM_THREADS", ns.c_str(), 1);
    setenv("MKL_NUM_THREADS", ns.c_str(), 1);
    setenv("OMP_WAIT_POLICY", speed == "hard" ? "ACTIVE" : "PASSIVE", 1);
#endif
}

bool runPiper(const fs::path& model, const fs::path& outWav, const std::string& text,
              std::string* err) {
    fs::path exe = piperExe();
    if (!fs::exists(exe)) {
        if (err) *err = "piper binary missing";
        return false;
    }
    applyPiperLoadEnv(stack::StackConfig::get().lectorSpeed);
    std::error_code ec;
    fs::create_directories(outWav.parent_path(), ec);
    fs::path txtPath = workDir() / ("cue_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    fs::create_directories(workDir(), ec);
    {
        std::ofstream t(txtPath, std::ios::binary | std::ios::trunc);
        if (!t) {
            if (err) *err = "cannot write cue text";
            return false;
        }
        t.write(text.data(), (std::streamsize)text.size());
        if (text.empty() || text.back() != '\n') t.put('\n');
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        if (err) *err = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

    // Feed stdin from the text file via redirected handle
    HANDLE txtRd = CreateFileW(txtPath.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (txtRd == INVALID_HANDLE_VALUE) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (err) *err = "cannot open cue text";
        return false;
    }

    std::wstring cmd = L"\"" + exe.wstring() + L"\" --model \"" + model.wstring() +
                       L"\" --output_file \"" + outWav.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = txtRd;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cwd = exe.parent_path().wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    CloseHandle(txtRd);
    if (!ok) {
        CloseHandle(rd);
        if (err) *err = "CreateProcess failed";
        return false;
    }
    // Drain pipe to avoid blocking
    char buf[512];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {}
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fs::remove(txtPath, ec);
    if (code != 0 || !fs::exists(outWav, ec)) {
        if (err) *err = "piper failed (" + std::to_string((int)code) + ")";
        return false;
    }
    return true;
#else
    std::string modelS = model.string();
    std::string outS = outWav.string();
    std::string exeS = exe.string();
    std::string txtS = txtPath.string();
    std::string cwd = exe.parent_path().string();
    const std::string speed = stack::StackConfig::get().lectorSpeed;
    const unsigned threads = piperThreadCount(speed);

    pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (chdir(cwd.c_str()) != 0) _exit(127);
        if (threads > 0) {
            const std::string threadsStr = std::to_string(threads);
            setenv("OMP_NUM_THREADS", threadsStr.c_str(), 1);
            setenv("OPENBLAS_NUM_THREADS", threadsStr.c_str(), 1);
            setenv("MKL_NUM_THREADS", threadsStr.c_str(), 1);
            setenv("OMP_WAIT_POLICY", speed == "hard" ? "ACTIVE" : "PASSIVE", 1);
        } else {
            unsetenv("OMP_NUM_THREADS");
            unsetenv("OPENBLAS_NUM_THREADS");
            unsetenv("MKL_NUM_THREADS");
            unsetenv("OMP_WAIT_POLICY");
        }
        int fd = open(txtS.c_str(), O_RDONLY);
        if (fd < 0) _exit(126);
        dup2(fd, STDIN_FILENO);
        close(fd);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execl(exeS.c_str(), "piper", "--model", modelS.c_str(), "--output_file", outS.c_str(),
              (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    fs::remove(txtPath, ec);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !fs::exists(outWav, ec)) {
        if (err) *err = "piper failed";
        return false;
    }
    return true;
#endif
}

bool extractEngineArchive(const fs::path& archive, std::string* err) {
    std::error_code ec;
    fs::create_directories(engineDir(), ec);
#ifdef _WIN32
    if (!zipwrite::unzipToDirectory(archive, engineDir(), err)) return false;
#else
    std::string cmd = "tar -xzf \"" + archive.string() + "\" -C \"" + engineDir().string() + "\"";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        if (err) *err = "tar extract failed";
        return false;
    }
#endif
    // Normalize: some archives extract to piper/, some flatten
    if (!fs::exists(piperExe(), ec)) {
        // Look for piper binary anywhere under engineDir
        for (auto& ent : fs::recursive_directory_iterator(engineDir(), ec)) {
            if (ec) break;
            if (!ent.is_regular_file(ec)) continue;
            auto name = ent.path().filename().string();
#ifdef _WIN32
            if (util::iequals(name, "piper.exe")) {
                fs::path destDir = engineDir() / "piper";
                fs::create_directories(destDir, ec);
                // Move sibling files too — keep parent folder if already named piper
                fs::path parent = ent.path().parent_path();
                if (parent != destDir) {
                    for (auto& sib : fs::directory_iterator(parent, ec)) {
                        fs::path target = destDir / sib.path().filename();
                        if (!fs::exists(target, ec))
                            fs::rename(sib.path(), target, ec);
                    }
                }
                break;
            }
#else
            if (name == "piper") {
                fs::path destDir = engineDir() / "piper";
                fs::create_directories(destDir, ec);
                fs::path parent = ent.path().parent_path();
                if (parent != destDir) {
                    for (auto& sib : fs::directory_iterator(parent, ec)) {
                        fs::path target = destDir / sib.path().filename();
                        if (!fs::exists(target, ec))
                            fs::rename(sib.path(), target, ec);
                    }
                }
                fs::permissions(piperExe(), fs::perms::owner_exec | fs::perms::group_exec |
                                                fs::perms::others_exec,
                                fs::perm_options::add, ec);
                break;
            }
#endif
        }
    }
#ifndef _WIN32
    if (fs::exists(piperExe(), ec))
        fs::permissions(piperExe(),
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
#endif
    if (!fs::exists(piperExe(), ec)) {
        if (err) *err = "piper binary not found after extract";
        return false;
    }
    return true;
}

std::string engineUrl() {
#ifdef _WIN32
    return std::string(kPiperBase) + "piper_windows_amd64.zip";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return std::string(kPiperBase) + "piper_linux_aarch64.tar.gz";
#else
    return std::string(kPiperBase) + "piper_linux_x86_64.tar.gz";
#endif
}

std::string engineArchiveName() {
#ifdef _WIN32
    return "piper_windows_amd64.zip";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "piper_linux_aarch64.tar.gz";
#else
    return "piper_linux_x86_64.tar.gz";
#endif
}

bool downloadEngine(uint64_t gen, std::string* err) {
    if (fs::exists(piperExe())) return true;
    TransferMeter meter;
    meter.begin(i18n::tr("lector.dl_engine"));
    std::string url = engineUrl();
    auto bytes = http::getBinaryLong(url, err, 300, [&](uint64_t got, uint64_t total) {
        meter.onProgress(got, total);
    });
    if (g_stop || gen != g_dlGen) return false;
    if (bytes.empty()) {
        if (err && err->empty()) *err = "engine download empty";
        return false;
    }
    fs::path archive = rootDir() / engineArchiveName();
    if (!writeBytes(archive, bytes)) {
        if (err) *err = "cannot save engine archive";
        return false;
    }
    setStatus(State::Downloading, i18n::tr("lector.setup_engine"));
    setStatusDetail(formatBytes(bytes.size()));
    g_progress = 0.f;
    if (!extractEngineArchive(archive, err)) return false;
    std::error_code ec;
    fs::remove(archive, ec);
    g_progress = 1.f;
    setStatusDetail({});
    return fs::exists(piperExe());
}

bool downloadVoiceFiles(const VoiceInfo& v, uint64_t gen, std::string* err) {
    fs::path dir = voicesDir() / v.id;
    fs::path onnx = dir / (v.id + ".onnx");
    fs::path json = dir / (v.id + ".onnx.json");
    if (fs::exists(onnx) && fs::exists(json)) return true;

    TransferMeter meter;
    meter.begin(std::string(i18n::tr("lector.dl_voice")) + " — " + v.name);
    std::string onnxUrl = std::string(kHfBase) + v.hfPath + "/" + v.id + ".onnx";
    std::string jsonUrl = std::string(kHfBase) + v.hfPath + "/" + v.id + ".onnx.json";

    auto onnxBytes = http::getBinaryLong(onnxUrl, err, 600, [&](uint64_t got, uint64_t total) {
        meter.onProgress(got, total);
    });
    if (g_stop || gen != g_dlGen) return false;
    if (onnxBytes.empty()) {
        if (err && err->empty()) *err = "voice onnx empty";
        return false;
    }

    meter.begin(std::string(i18n::tr("lector.dl_voice_cfg")) + " — " + v.name);
    auto jsonBytes = http::getBinaryLong(jsonUrl, err, 60, [&](uint64_t got, uint64_t total) {
        meter.onProgress(got, total);
    });
    if (g_stop || gen != g_dlGen) return false;
    if (jsonBytes.empty()) {
        if (err && err->empty()) *err = "voice json empty";
        return false;
    }
    setStatus(State::Downloading, i18n::tr("lector.saving_voice"));
    setStatusDetail(v.name);
    if (!writeBytes(onnx, onnxBytes) || !writeBytes(json, jsonBytes)) {
        if (err) *err = "cannot save voice";
        return false;
    }
    g_progress = 1.f;
    setStatusDetail({});
    return true;
}

void refreshStateLocked() {
    auto& cfg = stack::StackConfig::get();
    if (!cfg.lectorEnabled) {
        g_state = State::Disabled;
        g_status = i18n::tr("lector.disabled");
        return;
    }
    if (g_dlBusy) {
        g_state = State::Downloading;
        return;
    }
    if (!fs::exists(piperExe())) {
        g_state = State::NeedEngine;
        g_status = i18n::tr("lector.need_engine");
        return;
    }
    std::string vid = cfg.lectorVoiceId;
    if (vid.empty() || !findVoice(vid)) {
        g_state = State::NeedVoice;
        g_status = i18n::tr("lector.need_voice");
        return;
    }
    if (!voiceReady(vid)) {
        g_state = State::NeedVoice;
        g_status = i18n::tr("lector.need_voice");
        return;
    }
    g_state = State::Ready;
    g_status = i18n::tr("lector.ready");
}

void startSetupJob(std::string voiceId) {
    if (g_dlBusy.exchange(true)) return;
    uint64_t gen = ++g_dlGen;
    clearError();
    g_progress = 0.f;
    setStatus(State::Downloading, i18n::tr("lector.starting"));
    core::enqueue([gen, voiceId = std::move(voiceId)]() {
        std::string err;
        bool ok = false;
        do {
            if (g_stop || gen != g_dlGen) break;
            if (!downloadEngine(gen, &err)) break;
            auto& cfg = stack::StackConfig::get();
            std::string vid = voiceId.empty() ? cfg.lectorVoiceId : voiceId;
            if (vid.empty()) {
                // Default by UI / subs language
                std::string lang = cfg.subsPreferredLang;
                if (lang.empty()) lang = cfg.uiLanguage;
                for (auto& v : voiceCatalog()) {
                    if (v.lang == lang) {
                        vid = v.id;
                        break;
                    }
                }
                if (vid.empty()) vid = "en_US-lessac-medium";
                cfg.lectorVoiceId = vid;
                cfg.save();
            }
            const VoiceInfo* v = findVoice(vid);
            if (!v) {
                err = "unknown voice";
                break;
            }
            if (!downloadVoiceFiles(*v, gen, &err)) break;
            ok = true;
        } while (false);

        g_dlBusy = false;
        g_progress = ok ? 1.f : 0.f;
        PendingGen pending;
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!ok) {
                g_state = State::Error;
                g_error = err.empty() ? i18n::tr("lector.setup_failed") : err;
                g_status = i18n::tr("lector.setup_failed");
                g_hasPendingGen = false;
            } else {
                g_error.clear();
                refreshStateLocked();
                if (g_hasPendingGen) {
                    pending = g_pendingGen;
                    hasPending = true;
                    g_hasPendingGen = false;
                }
            }
        }
        if (hasPending && ok)
            startGenerate(pending.video, pending.srt, pending.voice);
    });
}

bool synthesizeToFile(const std::string& voiceId, const std::string& text, const fs::path& out,
                      std::string* err) {
    fs::path model = voicesDir() / voiceId / (voiceId + ".onnx");
    if (!fs::exists(model)) {
        if (err) *err = "voice model missing";
        return false;
    }
    return runPiper(model, out, text, err);
}

bool buildTimedTrack(const std::string& voiceId, const std::vector<Cue>& cues,
                     const fs::path& outWav, uint64_t gen, std::string* err) {
    if (cues.empty()) {
        if (err) *err = i18n::tr("lector.no_cues");
        return false;
    }
    WavData master;
    master.sampleRate = 22050;
    master.channels = 1;
    const int64_t lastEnd = cues.back().endMs + 1500;
    const size_t totalSamples =
        (size_t)std::max<int64_t>(1, (lastEnd * master.sampleRate) / 1000);
    master.pcm.assign(totalSamples, 0);

    fs::path tmpDir = workDir() / ("job_" + std::to_string(gen));
    std::error_code ec;
    fs::create_directories(tmpDir, ec);

    for (size_t i = 0; i < cues.size(); ++i) {
        if (g_stop || gen != g_jobGen) {
            if (err) *err = "cancelled";
            fs::remove_all(tmpDir, ec);
            return false;
        }
        g_jobProgress = (float)i / (float)cues.size();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_jobMsg = std::string(i18n::tr("lector.gen_cue")) + " " + std::to_string(i + 1) +
                       "/" + std::to_string(cues.size());
        }
        fs::path cueWav = tmpDir / ("cue_" + std::to_string(i) + ".wav");
        if (!synthesizeToFile(voiceId, cues[i].text, cueWav, err)) {
            fs::remove_all(tmpDir, ec);
            return false;
        }
        WavData w;
        if (!readWav(cueWav, w)) {
            if (err) *err = "bad cue wav";
            fs::remove_all(tmpDir, ec);
            return false;
        }
        if (w.sampleRate != master.sampleRate || w.channels != master.channels) {
            // Accept first cue rate if different
            if (i == 0) {
                master.sampleRate = w.sampleRate;
                master.channels = w.channels;
                const size_t n =
                    (size_t)std::max<int64_t>(1, (lastEnd * master.sampleRate) / 1000) *
                    (size_t)master.channels;
                master.pcm.assign(n, 0);
            }
        }
        const size_t start =
            (size_t)((cues[i].startMs * (int64_t)master.sampleRate) / 1000) *
            (size_t)master.channels;
        for (size_t s = 0; s < w.pcm.size(); ++s) {
            size_t dst = start + s;
            if (dst >= master.pcm.size()) break;
            int v = (int)master.pcm[dst] + (int)w.pcm[s];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            master.pcm[dst] = (int16_t)v;
        }
        fs::remove(cueWav, ec);
    }

    g_jobProgress = 0.98f;
    if (!writeWav(outWav, master)) {
        if (err) *err = "cannot write lector wav";
        fs::remove_all(tmpDir, ec);
        return false;
    }
    fs::remove_all(tmpDir, ec);
    g_jobProgress = 1.f;
    return true;
}

} // namespace

const std::vector<VoiceInfo>& catalog() { return voiceCatalog(); }

std::vector<VoiceInfo> voicesForLang(const std::string& lang) {
    std::vector<VoiceInfo> out;
    for (auto& v : voiceCatalog())
        if (lang.empty() || v.lang == lang) out.push_back(v);
    return out;
}

void init() {
    g_stop = false;
    std::error_code ec;
    fs::create_directories(rootDir(), ec);
    fs::create_directories(voicesDir(), ec);
    fs::create_directories(samplesDir(), ec);
    fs::create_directories(workDir(), ec);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        refreshStateLocked();
    }
    auto& cfg = stack::StackConfig::get();
    if (cfg.lectorEnabled)
        ensureSetup(cfg.lectorVoiceId);
}

void shutdown() {
    g_stop = true;
    ++g_dlGen;
    ++g_jobGen;
    g_dlBusy = false;
    g_jobBusy = false;
}

void tick() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_dlBusy && !g_jobBusy) refreshStateLocked();
}

State state() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state;
}

float progress() { return g_progress.load(); }

std::string statusMessage() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

std::string statusDetail() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_statusDetail;
}

std::string errorMessage() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_error;
}

bool engineReady() { return fs::exists(piperExe()); }

bool voiceReady(const std::string& voiceId) {
    if (voiceId.empty()) return false;
    fs::path dir = voicesDir() / voiceId;
    return fs::exists(dir / (voiceId + ".onnx")) && fs::exists(dir / (voiceId + ".onnx.json"));
}

std::string voicePath(const std::string& voiceId) {
    if (!voiceReady(voiceId)) return {};
    return (voicesDir() / voiceId / (voiceId + ".onnx")).string();
}

void ensureSetup(const std::string& voiceId) {
    auto& cfg = stack::StackConfig::get();
    if (!cfg.lectorEnabled) return;
    std::string vid = voiceId.empty() ? cfg.lectorVoiceId : voiceId;
    if (engineReady() && !vid.empty() && voiceReady(vid)) {
        std::lock_guard<std::mutex> lk(g_mu);
        refreshStateLocked();
        return;
    }
    startSetupJob(vid);
}

void downloadVoice(const std::string& voiceId) {
    if (voiceId.empty() || !findVoice(voiceId)) return;
    auto& cfg = stack::StackConfig::get();
    cfg.lectorVoiceId = voiceId;
    cfg.save();
    ensureSetup(voiceId);
}

void playSample(const std::string& voiceId) {
    if (!voiceReady(voiceId) || !engineReady()) {
        downloadVoice(voiceId);
        return;
    }
    const VoiceInfo* v = findVoice(voiceId);
    if (!v) return;
    fs::path out = samplesDir() / (voiceId + ".wav");
    if (fs::exists(out)) return; // already have sample; UI plays samplePath
    core::enqueue([voiceId, lang = v->lang, out]() {
        std::string err;
        synthesizeToFile(voiceId, samplePhrase(lang), out, &err);
    });
}

std::string samplePath(const std::string& voiceId) {
    fs::path p = samplesDir() / (voiceId + ".wav");
    std::error_code ec;
    if (fs::exists(p, ec)) return p.string();
    return {};
}

std::string findSubtitle(const std::string& videoPath, const std::string& preferredLang) {
    fs::path video(videoPath);
    if (video.empty()) return {};
    std::error_code ec;
    if (preferredLang.size() >= 2) {
        fs::path pref = video.parent_path() / (video.stem().string() + "." + preferredLang + ".srt");
        if (fs::exists(pref, ec)) return pref.string();
    }
    // Any lang-tagged sidecar matching stem
    if (fs::exists(video.parent_path(), ec)) {
        std::string stem = video.stem().string();
        std::string best;
        for (auto& ent : fs::directory_iterator(video.parent_path(), ec)) {
            if (!ent.is_regular_file(ec)) continue;
            if (util::lower(ent.path().extension().string()) != ".srt") continue;
            std::string name = ent.path().stem().string(); // e.g. Movie.en
            if (name == stem || name.rfind(stem + ".", 0) == 0) {
                if (best.empty() || name == stem + "." + preferredLang) best = ent.path().string();
            }
        }
        if (!best.empty()) return best;
    }
    return {};
}

std::string outputPathFor(const std::string& videoPath, const std::string& voiceId) {
    fs::path video(videoPath);
    std::string safe = voiceId;
    for (char& c : safe)
        if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
    return (video.parent_path() / (video.stem().string() + ".lector." + safe + ".wav")).string();
}

std::string fileUri(const std::string& path) {
    if (path.empty()) return {};
    if (path.rfind("file:", 0) == 0 || path.rfind("http://", 0) == 0 ||
        path.rfind("https://", 0) == 0)
        return path;
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
#ifdef _WIN32
    // file:///C:/...
    if (p.size() >= 2 && p[1] == ':')
        return "file:///" + p;
    return "file:///" + p;
#else
    if (!p.empty() && p[0] == '/') return "file://" + p;
    return "file://" + p;
#endif
}

void startGenerate(const std::string& videoPath, const std::string& srtPath,
                   const std::string& voiceId) {
    if (videoPath.empty() || srtPath.empty() || voiceId.empty()) return;
    if (!engineReady() || !voiceReady(voiceId)) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_pendingGen = PendingGen{videoPath, srtPath, voiceId};
            g_hasPendingGen = true;
            g_jobMsg = i18n::tr("lector.starting");
        }
        ensureSetup(voiceId);
        return;
    }
    if (g_jobBusy.exchange(true)) return;
    uint64_t gen = ++g_jobGen;
    g_jobProgress = 0.f;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_jobErr.clear();
        g_jobOut = outputPathFor(videoPath, voiceId);
        g_jobMsg = i18n::tr("lector.gen_start");
    }
    core::enqueue([gen, videoPath, srtPath, voiceId]() {
        std::string err;
        bool ok = false;
        fs::path out = outputPathFor(videoPath, voiceId);
        do {
            auto cues = parseSrt(srtPath);
            if (cues.empty()) {
                err = i18n::tr("lector.no_cues");
                break;
            }
            ok = buildTimedTrack(voiceId, cues, out, gen, &err);
        } while (false);

        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (ok) {
                g_jobOut = out.string();
                g_jobMsg = i18n::tr("lector.gen_done");
                g_jobErr.clear();
            } else {
                g_jobErr = err.empty() ? i18n::tr("lector.gen_failed") : err;
                g_jobMsg = g_jobErr;
            }
        }
        g_jobBusy = false;
        if (ok) g_jobProgress = 1.f;
    });
}

bool jobBusy() { return g_jobBusy.load(); }
float jobProgress() { return g_jobProgress.load(); }

std::string jobMessage() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_jobMsg;
}

std::string jobOutputPath() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_jobOut;
}

std::string jobError() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_jobErr;
}

} // namespace lector
