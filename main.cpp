// VeeaStats - a small Linux hardware monitor built with Dear ImGui + GLFW + OpenGL.
//
// How this file is organised:
//   1. Helpers         - reading files, parsing numbers, formatting text
//   2. Data types      - plain structs that hold what we display
//   3. Static info     - read ONCE at startup (CPU name, drives, OS, ...)
//   4. Live stats      - re-read every update interval (CPU load, RAM, GPU, ...)
//   5. UI              - settings, shared widgets and the Classic skin
//   6. HUD skins       - JARV, Galactic Conflict, Federation Gunship, Halloween,
//                        Poseidon, Otaku, Cyber-Punk and Classroom: one shared layout,
//                        each skin drawing it its own way
//   7. Skin switching  - fonts, styles and picking which skin draws the window
//   8. Overlay         - the Ctrl+Shift+O in-game overlay, its hotkey, and
//                        running in the background
//   9. main()          - window setup and the main loop
//
// Everything is read straight from /proc and /sys (no shell commands in the
// refresh loop), so it works the same on any Linux distro and costs almost
// nothing to run.

// Optional: build with -DIMGUI_IMPL_OPENGL_ES3 for devices that only have
// OpenGL ES (some ARM handhelds). Desktop OpenGL is the default.
#if defined(IMGUI_IMPL_OPENGL_ES3)
#define GLFW_INCLUDE_ES3
#endif

#define IMGUI_DEFINE_MATH_OPERATORS   // lets ImVec2 values be added and subtracted with + and -
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// The overlay is an X11 window (see section 8). Only headers here: libX11 and
// GLib are loaded at runtime, so they aren't needed to start the program.
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <gio/gio.h>

// Fonts for the HUD skins, embedded so nothing needs installing (SIL Open Font
// License; see fonts/). Trimmed to Western European characters to keep them small.
#include "fonts/chakrapetch_bold.h"
#include "fonts/cinzel_bold.h"
#include "fonts/delagothicone_regular.h"
#include "fonts/fredoka_semibold.h"
#include "fonts/hennypenny_regular.h"
#include "fonts/inter_bold.h"
#include "fonts/inter_regular.h"
#include "fonts/marcellus_regular.h"
#include "fonts/michroma_regular.h"
#include "fonts/mplusrounded1c_extrabold.h"
#include "fonts/orbitron_semibold.h"
#include "fonts/pressstart2p_regular.h"
#include "fonts/rajdhani_semibold.h"
#include "fonts/saira_semicondensed_medium.h"
#include "fonts/tiny5_regular.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace {

// ============================================================================
// 1. HELPERS
// ============================================================================

// How often live stats update, chosen from the dropdown at the top of the window.
// 250 ms is the lowest on offer: below that the monitor itself starts using
// noticeable CPU on small devices, and the kernel's CPU counters only tick every
// 10 ms, so shorter intervals would just make the CPU bars jumpy.
constexpr int kDefaultRefreshMs = 500;
constexpr int kRefreshChoicesMs[] = {250, 500, 750, 1000, 1250, 1500, 1750, 2000};

constexpr float kPi = 3.14159265f;

constexpr double kAnimationFrameSeconds = 1.0 / 30.0;   // frame time while a skin animates

// CPU voltage is read less often than everything else. On desktop boards it
// comes from a motherboard sensor chip, and each fresh reading costs the kernel
// 30-50 ms on a slow bus - most of this program's CPU use if done every refresh.
// The trade-off: the VCore figure lags up to this long behind.
constexpr double kVoltageReadIntervalMs = 5000.0;

constexpr double kBytesPerGb     = 1024.0 * 1024.0 * 1024.0;
constexpr double kKbPerGb        = 1024.0 * 1024.0;

constexpr bool kKeepGoing = true;   // return values for the line callbacks below
constexpr bool kStop      = false;

constexpr const char* kAmdVendorId    = "0x1002";   // PCI vendor ids as shown in sysfs
constexpr const char* kNvidiaVendorId = "0x10de";
constexpr const char* kIntelVendorId  = "0x8086";

// printf that returns a std::string.
__attribute__((format(printf, 1, 2)))
std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int length = vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    std::string result;
    if (length > 0) {
        result.resize(static_cast<size_t>(length));
        vsnprintf(&result[0], static_cast<size_t>(length) + 1, fmt, args);
    }
    va_end(args);
    return result;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Removes spaces, tabs, newlines and double quotes from both ends.
// (Quotes are stripped so values like PRETTY_NAME="Arch Linux" come out clean.)
std::string trim(std::string_view text) {
    constexpr const char* kJunk = " \t\r\n\"";
    const size_t first = text.find_first_not_of(kJunk);
    if (first == std::string_view::npos) return "";
    const size_t last = text.find_last_not_of(kJunk);
    return std::string(text.substr(first, last - first + 1));
}

// For lines like "model name : Intel(R) Core(TM) i7" returns the part after ':'.
std::string valueAfterColon(std::string_view line) {
    const size_t colon = line.find(':');
    return colon == std::string_view::npos ? "" : trim(line.substr(colon + 1));
}

bool fileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

// A per-user folder from the XDG spec: $`variable` if it's an absolute path (the
// spec says relative ones are to be ignored), else ~/`fallback`. "" if neither.
std::string xdgDirectory(const char* variable, const char* fallback) {
    const char* value = getenv(variable);
    if (value && value[0] == '/') return value;
    const char* home = getenv("HOME");
    return (home && *home) ? std::string(home) + "/" + fallback : "";
}

// The full path of program `name` found on $PATH, or "" (replaces `which`).
// Empty PATH entries are skipped: they mean "the current folder", from which no
// program should ever be run. Programs are started by this path (not looked up
// again by name), so what was checked is what runs.
std::string findCommand(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return "";

    std::string_view dirs(pathEnv);
    while (!dirs.empty()) {
        const size_t colon = dirs.find(':');
        const std::string dir(dirs.substr(0, colon));
        if (!dir.empty() && dir[0] == '/') {
            const std::string path = dir + "/" + name;
            if (access(path.c_str(), X_OK) == 0) return path;
        }
        if (colon == std::string_view::npos) break;
        dirs.remove_prefix(colon + 1);
    }
    return "";
}

bool commandExists(const char* name) { return !findCommand(name).empty(); }

// Calls onLine(const char* line) for every line of an open stream (newline removed).
// The callback returns kKeepGoing to continue or kStop to stop early.
template <typename Callback>
void forEachLineIn(FILE* stream, Callback onLine) {
    char* buffer = nullptr;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&buffer, &capacity, stream)) != -1) {
        if (length > 0 && buffer[length - 1] == '\n') buffer[length - 1] = '\0';
        if (onLine(static_cast<const char*>(buffer)) == kStop) break;
    }
    free(buffer);
}

// Same, for a file path. Returns false if the file could not be opened.
template <typename Callback>
bool forEachLine(const std::string& path, Callback onLine) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;
    forEachLineIn(file, onLine);
    fclose(file);
    return true;
}

// Same, for the output of a shell command. Only used for the two things that
// have no file-based alternative (RAM module details and rpm package counts),
// and only once at startup.
template <typename Callback>
void forEachCommandLine(const char* command, Callback onLine) {
    FILE* pipe = popen(command, "r");
    if (!pipe) return;
    forEachLineIn(pipe, onLine);
    pclose(pipe);
}

// Makes reads/writes on `fd` return straight away instead of waiting, keeping
// its other flags.
void setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Ends child process `pid` (with `group`, also every process it started, in
// the process group it leads), waiting about a second at most: a child stuck
// in a driver call must not freeze the window or keep VeeaStats from quitting.
// SIGTERM first, then SIGKILL. A child still there after that is stuck inside
// the kernel and can't be ended from here; it's left behind (and reaped by
// init once VeeaStats exits).
void endChildProcess(pid_t pid, bool group) {
    for (const int signal : {SIGTERM, SIGKILL}) {
        kill(group ? -pid : pid, signal);
        for (int wait = 0; wait < 50; ++wait) {
            if (waitpid(pid, nullptr, WNOHANG) != 0) return;   // gone (or not ours to wait for)
            usleep(10 * 1000);
        }
    }
}

// First line of a (small) file such as a sysfs entry, trimmed. "" if unreadable.
std::string readFirstLine(const std::string& path) {
    std::string result;
    forEachLine(path, [&](const char* line) {
        result = trim(line);
        return kStop;
    });
    return result;
}

// Parses a number from text. Returns false for things like "" or "[N/A]".
bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return false;   // "nan" and "inf" parse too, but aren't numbers we can show
    out = value;
    return true;
}

bool readNumber(const std::string& path, double& out) {
    return parseNumber(readFirstLine(path), out);
}

// Sorted directory listing. Never throws: a missing or unreadable folder
// simply gives an empty list.
std::vector<fs::path> listDirectory(const std::string& path) {
    std::vector<fs::path> entries;
    std::error_code error;
    for (fs::directory_iterator it(path, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

int countSubdirectories(const std::string& path) {
    int count = 0;
    for (const fs::path& entry : listDirectory(path)) {
        std::error_code error;
        if (fs::is_directory(entry, error)) ++count;
    }
    return count;
}

// The paths from the list that exist, in the same order. Used when the same
// value lives in different files depending on the driver or kernel version.
std::vector<std::string> existingPaths(const std::vector<std::string>& candidates) {
    std::vector<std::string> found;
    for (const std::string& path : candidates) {
        if (fileExists(path)) found.push_back(path);
    }
    return found;
}

// The first path from the list that exists, or "" if none do.
std::string firstExisting(const std::vector<std::string>& candidates) {
    const std::vector<std::string> found = existingPaths(candidates);
    return found.empty() ? "" : found.front();
}

// Reads a voltage sensor file (millivolts) and returns volts. Returns 0 if the
// file is missing or holds an implausible number (some sensors report junk).
double readVoltage(const std::string& path) {
    double millivolts = 0.0;
    if (path.empty() || !readNumber(path, millivolts)) return 0.0;
    return (millivolts > 100.0 && millivolts < 2500.0) ? millivolts / 1000.0 : 0.0;
}

// A temperature that isn't known (no sensor). NaN, not 0: below-freezing
// readings are real (a board outdoors, a cold start).
constexpr double kNoTemperature = std::numeric_limits<double>::quiet_NaN();
constexpr double kMinTemperatureC = -40.0, kMaxTemperatureC = 150.0;   // anything outside is a broken sensor

bool hasTemperature(double celsius) { return !std::isnan(celsius); }

// True for a plausible sensor reading. Exactly 0 is left out: that's what
// many drivers report for a sensor that is switched off or missing.
bool isRealTemperature(double celsius) { return celsius > kMinTemperatureC && celsius < kMaxTemperatureC && celsius != 0.0; }

// Reads a temperature sensor file (millidegrees Celsius) and returns °C. Returns
// 0 if the file is missing or holds an implausible number (unused inputs read 0,
// and some report junk such as -128).
double readTemperature(const std::string& path) {
    double millidegrees = 0.0;
    if (path.empty() || !readNumber(path, millidegrees)) return kNoTemperature;
    const double celsius = millidegrees / 1000.0;
    return isRealTemperature(celsius) ? celsius : kNoTemperature;
}

// The temperature file of the first thermal zone whose type contains one of
// `typeParts` ("x86_pkg_temp", "cpu-thermal", "gpu_thermal", ...), or "".
std::string findThermalZone(const std::string& sysRoot, std::initializer_list<const char*> typeParts) {
    for (const fs::path& zone : listDirectory(sysRoot + "/class/thermal")) {
        const std::string type = readFirstLine(zone.string() + "/type");
        for (const char* part : typeParts) {
            if (type.find(part) != std::string::npos && fileExists(zone.string() + "/temp")) return zone.string() + "/temp";
        }
    }
    return "";
}

// Milliseconds on a clock that never jumps backwards (for measuring intervals).
double monotonicMs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1e6;
}

// Device-tree files such as "compatible" hold several strings separated by NUL
// bytes (e.g. "rockchip,rk3568-mali" NUL "arm,mali-bifrost"). Returns them as a list.
std::vector<std::string> readNulSeparatedStrings(const std::string& path) {
    std::vector<std::string> parts;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return parts;
    char buffer[512];
    const size_t length = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);

    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i == length || buffer[i] == '\0') {
            if (i > start) parts.emplace_back(buffer + start, i - start);
            start = i + 1;
        }
    }
    return parts;
}

// ============================================================================
// 2. DATA TYPES
// ============================================================================

enum class GpuVendor { Unknown, Nvidia, Amd, Intel, Mali };

// Cumulative CPU time counters (in "jiffies") for one core, from /proc/stat.
struct CpuTimes {
    int id{-1};                      // the core's number: "cpu<id>" in /proc/stat
    unsigned long long active{0};    // ticks spent working
    unsigned long long idle{0};      // ticks spent idle
    unsigned long long iowait{0};    // ticks spent idle waiting for a disk (kept apart: the kernel may lower it)
};

struct RamStats {
    double totalGb{0.0};
    double usedGb{0.0};
    float percent{0.0f};
};

// What we know about the GPU right now. Different GPUs report different things,
// so the has... flags say which numbers are real, and the notes explain to the
// user why a bar is missing.
struct GpuStats {
    GpuVendor vendor{GpuVendor::Unknown};
    std::string name{"Unknown GPU"};
    float gpuUsage{0.0f};
    float vramUsage{0.0f};
    double vramUsedGb{0.0};
    double vramTotalGb{0.0};
    double clockGhz{0.0};
    double voltageV{0.0};
    double temperatureC{kNoTemperature};
    bool hasLoad{false};          // gpuUsage is valid
    bool loadIsEstimate{false};   // ...but only approximate (Intel)
    bool hasMemory{false};        // the vram... numbers are valid
    std::string loadNote;         // shown instead of the load bar when hasLoad is false
    std::string memoryNote;       // shown instead of the VRAM bar when hasMemory is false
};

struct DriveInfo {
    std::string deviceName;
    std::string model;
    double sizeGb{0.0};
    std::string type;   // NVMe SSD, SSD, HDD, SD/eMMC
};

struct MotherboardInfo {
    std::string name{"Unknown Motherboard"};
    std::string chipset{"Unknown Chipset"};
};

struct SystemInfo {
    std::string osName;
    std::string kernel;
    std::string packages;
    std::string shell;
};

// How full one storage device is. Space is only known for mounted filesystems,
// so used and free are summed over the filesystems mounted from this device.
struct DiskUsage {
    DriveInfo drive;
    double usedGb{0.0};
    double freeGb{0.0};                     // space a normal user can still fill
    std::vector<std::string> mountPoints;   // where each of its filesystems is mounted
};

// Things that never change while the program runs.
struct StaticInfo {
    std::string cpuModel;
    MotherboardInfo board;
    std::vector<DriveInfo> drives;
    std::vector<std::string> ramModules;   // ready-to-display text, one per DIMM
    SystemInfo os;
};

// Things that are re-read every refresh.
struct LiveStats {
    std::vector<float> cpuPercent;   // one entry per online core
    std::vector<int> cpuIds;         // each one's number (cores can be offline, so not always 0, 1, 2...)
    double cpuFreqGhz{0.0};
    double cpuVoltage{0.0};
    double cpuTemperatureC{kNoTemperature};
    RamStats ram;
    GpuStats gpu;
    std::string uptime{"N/A"};
};

// Load across all cores, 0-100.
float averageCpuPercent(const LiveStats& live) {
    if (live.cpuPercent.empty()) return 0.0f;
    float total = 0.0f;
    for (const float percent : live.cpuPercent) total += percent;
    return total / static_cast<float>(live.cpuPercent.size());
}

// ============================================================================
// 3. STATIC INFO (read once at startup)
// ============================================================================

// ---- CPU -------------------------------------------------------------------

std::string readCpuModelName() {
    // x86 uses "model name". Many ARM kernels only provide "Hardware" or "Processor".
    std::string modelName, fallback;
    forEachLine("/proc/cpuinfo", [&](const char* line) {
        if (startsWith(line, "model name")) {
            modelName = valueAfterColon(line);
            return kStop;
        }
        if (fallback.empty() && (startsWith(line, "Hardware") || startsWith(line, "Processor"))) {
            fallback = valueAfterColon(line);
        }
        return kKeepGoing;
    });
    if (!modelName.empty()) return modelName;
    if (!fallback.empty()) return fallback;
    return "CPU Monitor";
}

// ---- Motherboard -----------------------------------------------------------

// Linux has no "chipset model" file, but retail board names almost always contain
// it ("PRIME B650M-A", "MAG Z790 TOMAHAWK", "TRX50 AERO D"). Returns e.g. "B650",
// "X670E", or "" if no word in the name looks like a chipset.
std::string chipsetFromBoardName(const std::string& boardName) {
    static const char* const kPrefixes[] = {"TRX", "WRX", "A", "B", "H", "Q", "X", "Z", "W"};

    std::string token;
    auto checkToken = [&]() -> std::string {
        for (const char* prefix : kPrefixes) {
            if (!startsWith(token, prefix)) continue;
            size_t pos = std::strlen(prefix);
            const size_t digitsStart = pos;
            while (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos]))) ++pos;
            const size_t digitCount = pos - digitsStart;
            // Only numbers chipsets really have, so laptop board names like
            // "X515EA" or "W65_W67RB" aren't taken for one: TRX40/WRX90; three
            // digits ending in 0 (B650, Z790, W680), or X299/X399; or two digits
            // from 6 up on older Intel boards (Z97, H81, X99).
            const std::string_view digits(token.data() + digitsStart, digitCount);
            const bool longPrefix = std::strlen(prefix) == 3;
            const bool plausible =
                longPrefix ? digitCount == 2
                : digitCount == 3 ? (digits.back() == '0' || (prefix[0] == 'X' && (digits == "299" || digits == "399")))
                : digitCount == 2 && digits[0] >= '6' && std::strchr("BHQXZ", prefix[0]) != nullptr;
            if (!plausible) continue;

            // "E" is part of the chipset (X670E); a single trailing letter after
            // that is a form factor (B650M = micro-ATX, B650I = mini-ITX).
            if (pos < token.size() && token[pos] == 'E') ++pos;
            const size_t chipsetEnd = pos;
            const size_t rest = token.size() - pos;
            if (rest > 1 || (rest == 1 && !std::isalpha(static_cast<unsigned char>(token[pos])))) {
                continue;
            }
            return token.substr(0, chipsetEnd);
        }
        return "";
    };

    for (size_t i = 0; i <= boardName.size(); ++i) {
        const char c = i < boardName.size() ? boardName[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            continue;
        }
        if (!token.empty()) {
            std::string chipset = checkToken();
            if (!chipset.empty()) return chipset;
            token.clear();
        }
    }
    return "";
}

// "AMD" / "Intel" from the PCI host bridge, so the chipset reads "AMD B650".
std::string platformVendor() {
    for (const fs::path& device : listDirectory("/sys/bus/pci/devices")) {
        if (!startsWith(readFirstLine(device.string() + "/class"), "0x0600")) continue;
        const std::string pciVendor = readFirstLine(device.string() + "/vendor");
        if (pciVendor == "0x1022") return "AMD";
        if (pciVendor == "0x8086") return "Intel";
    }
    return "";
}

// Fallback when the board name doesn't help: some chipset families can be
// recognised by their PCI IDs (only the family, e.g. B650 and X670 share IDs).
std::string chipsetFamilyFromPci() {
    for (const fs::path& device : listDirectory("/sys/bus/pci/devices")) {
        if (readFirstLine(device.string() + "/vendor") != "0x1022") continue;
        const std::string pciDevice = readFirstLine(device.string() + "/device");
        if (pciDevice == "0x43f6" || pciDevice == "0x43f7") return "AMD 600/800 Series";
        if (pciDevice == "0x43eb" || pciDevice == "0x43ee") return "AMD 500 Series";
    }
    return "";
}

MotherboardInfo readMotherboardInfo() {
    MotherboardInfo board;
    const std::string vendor  = readFirstLine("/sys/class/dmi/id/board_vendor");
    const std::string product = readFirstLine("/sys/class/dmi/id/board_name");

    if (!vendor.empty() || !product.empty()) {
        board.name = trim(vendor + " " + product);
    } else {
        // ARM boards have no DMI table; the device tree holds the board name instead.
        const std::string deviceTreeModel = readFirstLine("/sys/firmware/devicetree/base/model");
        if (!deviceTreeModel.empty()) board.name = deviceTreeModel;
    }

    const std::string chipset = chipsetFromBoardName(product);
    if (!chipset.empty()) {
        board.chipset = trim(platformVendor() + " " + chipset);
        return board;
    }

    const std::string family = chipsetFamilyFromPci();
    if (!family.empty()) {
        board.chipset = family;
        return board;
    }

    // Unknown chipset: describe the PCI host/ISA bridge instead.
    for (const fs::path& device : listDirectory("/sys/bus/pci/devices")) {
        const std::string classCode = readFirstLine(device.string() + "/class");
        if (startsWith(classCode, "0x0600") || startsWith(classCode, "0x0601")) {
            const std::string pciVendor = readFirstLine(device.string() + "/vendor");
            const std::string pciDevice = readFirstLine(device.string() + "/device");
            if (!pciVendor.empty() && !pciDevice.empty()) {
                board.chipset = "PCI Host/ISA Bridge [" + pciVendor + ":" + pciDevice + "]";
                break;
            }
        }
    }
    return board;
}

// ---- Storage ---------------------------------------------------------------

std::vector<DriveInfo> readStorageDrives() {
    static const char* const kIgnoredPrefixes[] = {"loop", "ram", "zram", "sr"};

    std::vector<DriveInfo> drives;
    for (const fs::path& entry : listDirectory("/sys/block")) {
        const std::string name = entry.filename().string();
        const std::string dir  = entry.string();

        bool ignored = false;
        for (const char* prefix : kIgnoredPrefixes) {
            if (startsWith(name, prefix)) ignored = true;
        }
        // SD/eMMC cards expose hidden helper devices (mmcblk0boot0, mmcblk0rpmb).
        if (startsWith(name, "mmcblk") && (name.find("boot") != std::string::npos ||
                                           name.find("rpmb") != std::string::npos)) {
            ignored = true;
        }
        if (ignored) continue;

        const bool isNvme = startsWith(name, "nvme");
        const bool isSdOrEmmc = startsWith(name, "mmcblk");

        // SATA/USB disks have "model"; NVMe and SD/eMMC devices use "name".
        std::string model = readFirstLine(dir + "/device/model");
        if (model.empty()) model = readFirstLine(dir + "/device/name");
        if (model.empty()) {
            if (!isNvme) continue;
            model = "NVMe Storage Device";
        }

        double sectors = 0.0;   // sysfs always counts in 512-byte sectors
        if (!readNumber(dir + "/size", sectors)) continue;
        const double sizeGb = sectors * 512.0 / kBytesPerGb;
        if (sizeGb <= 0.0) continue;

        std::string type = "SSD";
        if (isNvme)                                           type = "NVMe SSD";
        else if (isSdOrEmmc)                                  type = "SD/eMMC";
        else if (readFirstLine(dir + "/queue/rotational") == "1") type = "HDD";

        drives.push_back({name, model, sizeGb, type});
    }
    return drives;
}

// ---- RAM modules -----------------------------------------------------------

struct RamModule {
    std::string manufacturer, partNumber, size, speed;
};

std::string describeRamModule(size_t index, const RamModule& module) {
    std::string label = "DIMM " + std::to_string(index + 1) + ": ";
    if (!module.manufacturer.empty() && module.manufacturer != "Unknown") {
        label += module.manufacturer + " ";
    }
    label += module.partNumber;
    if (!module.size.empty() || !module.speed.empty()) {
        label += " (" + module.size + (module.speed.empty() ? "" : " @ " + module.speed) + ")";
    }
    return label;
}

// DIMM details live in the BIOS tables, which only root can read, so dmidecode
// is only run when VeeaStats itself runs as root. (Not through "sudo -n": every
// launch would log a failed root attempt in the system journal, and where sudo
// needs no password it would quietly run a root program nobody agreed to.)
// Otherwise the UI just shows "Standard System RAM".
std::vector<std::string> readRamModuleLabels() {
    const std::string dmidecode = (geteuid() == 0) ? findCommand("dmidecode") : "";
    const bool canRead = !dmidecode.empty() && dmidecode.find('\'') == std::string::npos;   // quoted below
    const std::string command = "'" + dmidecode + "' -t memory 2>/dev/null";
    std::vector<RamModule> modules;
    RamModule current;
    bool inDevice = false;

    auto finishDevice = [&]() {
        const bool empty = current.partNumber.empty() || current.partNumber == "NO DIMM" ||
                           startsWith(current.size, "No Module");
        if (inDevice && !empty) modules.push_back(current);
        current = RamModule();
    };

    if (canRead) forEachCommandLine(command.c_str(), [&](const char* line) {
        const std::string text = trim(line);
        if (text == "Memory Device") {
            finishDevice();
            inDevice = true;
        } else if (inDevice) {
            if (startsWith(text, "Manufacturer:"))      current.manufacturer = valueAfterColon(text);
            else if (startsWith(text, "Part Number:"))  current.partNumber = valueAfterColon(text);
            else if (startsWith(text, "Size:"))         current.size = valueAfterColon(text);
            else if (startsWith(text, "Speed:"))        current.speed = valueAfterColon(text);   // "Configured Memory Speed:" is deliberately not matched
        }
        return kKeepGoing;
    });
    finishDevice();

    std::vector<std::string> labels;
    for (size_t i = 0; i < modules.size(); ++i) labels.push_back(describeRamModule(i, modules[i]));

    if (labels.empty()) {
        // Fallback: ECC-capable systems expose DIMM labels through the EDAC driver.
        for (const fs::path& controller : listDirectory("/sys/devices/system/edac/mc")) {
            for (const fs::path& dimm : listDirectory(controller.string())) {
                const std::string label = readFirstLine(dimm.string() + "/dimm_label");
                if (!label.empty()) labels.push_back(describeRamModule(labels.size(), {"Generic", label, "", ""}));
            }
        }
    }
    return labels;
}

// ---- Operating system ------------------------------------------------------

std::string readOsName() {
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::string name;
        forEachLine(path, [&](const char* line) {
            if (startsWith(line, "PRETTY_NAME=")) {
                name = trim(line + strlen("PRETTY_NAME="));
                return kStop;
            }
            return kKeepGoing;
        });
        if (!name.empty()) return name;
    }
    return "Linux System";
}

std::string readKernelVersion() {
    utsname info;
    if (uname(&info) == 0) return std::string(info.sysname) + " " + info.release;
    return "Linux Kernel";
}

std::string readShellName() {
    const char* shell = getenv("SHELL");
    if (!shell || !*shell) return "Unknown Shell";
    const char* lastSlash = strrchr(shell, '/');
    return lastSlash ? lastSlash + 1 : shell;
}

// ---- Package counts --------------------------------------------------------
// Counted by looking at each package manager's database directly. This is
// instant, unlike running the package manager itself (which can take seconds).
// To support another package manager: write a countXxx() function and add it
// to the table in readPackageSummary().

int countPacman() {
    return countSubdirectories("/var/lib/pacman/local");   // one folder per installed package
}

int countDpkg() {
    int count = 0;
    forEachLine("/var/lib/dpkg/status", [&](const char* line) {
        // "Status: install ok installed", or "hold ok installed" for a held package.
        if (startsWith(line, "Status: ") && endsWith(line, " ok installed")) ++count;
        return kKeepGoing;
    });
    return count;
}

int countRpm() {
    // The rpm database is a binary format, so ask rpm itself.
    const std::string rpm = findCommand("rpm");
    if (rpm.empty() || rpm.find('\'') != std::string::npos) return 0;   // quoted below
    int count = 0;
    forEachCommandLine(("'" + rpm + "' -qa 2>/dev/null").c_str(), [&](const char*) {
        ++count;
        return kKeepGoing;
    });
    return count;
}

int countFlatpak() {
    // Counts installed apps (not runtimes), system-wide and per-user.
    int count = countSubdirectories("/var/lib/flatpak/app");   // note: only app *ids*, not arch/branch
    const std::string dataHome = xdgDirectory("XDG_DATA_HOME", ".local/share");
    if (!dataHome.empty()) count += countSubdirectories(dataHome + "/flatpak/app");
    return count;
}

int countSnap() {
    // Every installed snap is mounted as a folder under /snap (plus a "bin" helper folder).
    for (const char* root : {"/snap", "/var/lib/snapd/snap"}) {
        const int folders = countSubdirectories(root);
        if (folders > 0) return folders - (fileExists(std::string(root) + "/bin") ? 1 : 0);
    }
    return 0;
}

std::string readPackageSummary() {
    struct PackageSource {
        const char* name;
        int (*count)();
    };
    static const PackageSource kSources[] = {
        {"dpkg", countDpkg}, {"pacman", countPacman}, {"rpm", countRpm},
        {"flatpak", countFlatpak}, {"snap", countSnap},
    };

    std::string summary;
    for (const PackageSource& source : kSources) {
        const int count = source.count();
        if (count <= 0) continue;
        if (!summary.empty()) summary += ", ";
        summary += format("%d (%s)", count, source.name);
    }
    return summary.empty() ? "N/A" : summary;
}

StaticInfo readStaticInfo() {
    StaticInfo info;
    info.cpuModel   = readCpuModelName();
    info.board      = readMotherboardInfo();
    info.drives     = readStorageDrives();
    info.ramModules = readRamModuleLabels();
    info.os = {readOsName(), readKernelVersion(), readPackageSummary(), readShellName()};
    return info;
}

// ============================================================================
// 4. LIVE STATS (re-read every refresh)
// ============================================================================

// ---- CPU -------------------------------------------------------------------

std::vector<CpuTimes> readCpuTimes() {
    std::vector<CpuTimes> cores;
    forEachLine("/proc/stat", [&](const char* line) {
        // The per-core lines come first: "cpu0 ...", "cpu1 ...". The very first
        // line, "cpu  ...", is the all-core total, which we skip.
        if (!startsWith(line, "cpu")) return kStop;   // past the CPU lines (and their long "intr" neighbour)
        if (!isdigit(static_cast<unsigned char>(line[3]))) return kKeepGoing;

        int id = -1;
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &id, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 5) {
            return kKeepGoing;
        }

        CpuTimes times;
        times.id = id;
        times.active = user + nice + system + irq + softirq + steal;   // guest time is already counted inside user/nice
        times.idle   = idle;
        times.iowait = iowait;
        cores.push_back(times);
        return kKeepGoing;
    });
    return cores;
}

double readCpuFreqGhz() {
    double khz = 0.0;
    if (readNumber("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", khz) && khz > 0.0) {
        return khz / 1e6;
    }

    // Fallback for systems without cpufreq: the first "cpu MHz" line in /proc/cpuinfo.
    double mhz = 0.0;
    forEachLine("/proc/cpuinfo", [&](const char* line) {
        if (!startsWith(line, "cpu MHz")) return kKeepGoing;
        mhz = strtod(valueAfterColon(line).c_str(), nullptr);
        return kStop;
    });
    return mhz / 1000.0;
}

// The hwmon drivers of CPUs' own sensors (AMD, AMD via zenpower, Intel, ARM boards).
constexpr const char* kCpuSensorNames[] = {"k10temp", "zenpower", "coretemp", "cpu_thermal"};

// Finds the sensor files that might report CPU core voltage, best guess first.
// Done once at startup so each refresh only reads a couple of tiny files.
//   1. Any voltage whose label names the CPU core (zenpower "SVI2_Core",
//      asus-ec-sensors "CPU Core", ...).
//   2. in0 of a motherboard Super I/O chip (Nuvoton "nct6799", ITE "it8686",
//      ...). Boards wire in0 to Vcore; Ryzen 7000+ CPUs report no voltage of
//      their own, so on those this is the only source.
//   3. Unlabelled inputs of CPU sensors.
std::vector<std::string> findCpuVoltageInputs() {
    static const char* const kSuperIoPrefixes[] = {"nct6", "it8", "w836"};
    static const char* const kCoreLabels[] = {"vcore", "cpu core", "svi2_core", "vddcr_cpu", "cpu vcore"};

    std::vector<std::string> labelled, superIo, cpuSensor;
    for (const fs::path& hwmon : listDirectory("/sys/class/hwmon")) {
        const std::string dir = hwmon.string();
        const std::string sensorName = readFirstLine(dir + "/name");

        for (int i = 0; i < 16; ++i) {
            std::string label = readFirstLine(format("%s/in%d_label", dir.c_str(), i));
            if (label.empty()) continue;
            for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::string input = format("%s/in%d_input", dir.c_str(), i);
            for (const char* coreLabel : kCoreLabels) {
                if (label == coreLabel && fileExists(input)) labelled.push_back(input);
            }
        }

        for (const char* prefix : kSuperIoPrefixes) {
            if (startsWith(sensorName, prefix) && fileExists(dir + "/in0_input")) {
                superIo.push_back(dir + "/in0_input");
            }
        }

        if (std::find(std::begin(kCpuSensorNames), std::end(kCpuSensorNames), sensorName) == std::end(kCpuSensorNames)) continue;
        for (const char* input : {"in0_input", "in1_input", "in2_input"}) {
            const std::string path = dir + "/" + input;
            if (fileExists(path)) cpuSensor.push_back(path);
        }
    }

    std::vector<std::string> inputs = labelled;
    inputs.insert(inputs.end(), superIo.begin(), superIo.end());
    inputs.insert(inputs.end(), cpuSensor.begin(), cpuSensor.end());
    return inputs;
}

double readCpuVoltage(const std::vector<std::string>& inputs) {
    for (const std::string& path : inputs) {
        const double volts = readVoltage(path);
        if (volts > 0.0) return volts;
    }
    return 0.0;
}

// Finds the sensor file for the CPU's temperature, best guess first. Done once
// at startup.
//   AMD:   k10temp or zenpower. "Tdie" is the die itself; some Ryzens also
//          report "Tctl", raised a few degrees for fan control, so Tdie wins.
//   Intel: coretemp's "Package id 0" (the whole chip, not a single core).
//   ARM boards: cpu_thermal.
//   Anything else: a thermal zone the kernel names after the CPU.
std::string findCpuTemperatureInput() {
    static const char* const kBestLabels[] = {"tdie", "tctl", "package id 0"};   // in order of preference
    constexpr int kUnlabelled = 3, kOtherLabel = 4;                               // then these

    std::string best;
    int bestRank = kOtherLabel + 1;
    for (const fs::path& hwmon : listDirectory("/sys/class/hwmon")) {
        const std::string dir = hwmon.string();
        const std::string sensorName = readFirstLine(dir + "/name");
        if (std::find(std::begin(kCpuSensorNames), std::end(kCpuSensorNames), sensorName) == std::end(kCpuSensorNames)) continue;

        for (int i = 1; i <= 16; ++i) {
            const std::string input = format("%s/temp%d_input", dir.c_str(), i);
            if (!fileExists(input)) continue;
            std::string label = readFirstLine(format("%s/temp%d_label", dir.c_str(), i));
            for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int rank = label.empty() ? kUnlabelled : kOtherLabel;
            for (int j = 0; j < 3; ++j) {
                if (label == kBestLabels[j]) rank = j;
            }
            if (rank < bestRank) {
                best = input;
                bestRank = rank;
            }
        }
    }
    return best.empty() ? findThermalZone("/sys", {"x86_pkg_temp", "cpu"}) : best;
}

// ---- RAM -------------------------------------------------------------------

RamStats readRamStats() {
    long long totalKb = 0, availableKb = 0;
    forEachLine("/proc/meminfo", [&](const char* line) {
        if (startsWith(line, "MemTotal:"))          sscanf(line + 9, "%lld", &totalKb);
        else if (startsWith(line, "MemAvailable:")) sscanf(line + 13, "%lld", &availableKb);
        return (totalKb > 0 && availableKb > 0) ? kStop : kKeepGoing;
    });

    RamStats ram;
    if (totalKb > 0) {
        const long long usedKb = totalKb - availableKb;
        ram.totalGb = static_cast<double>(totalKb) / kKbPerGb;
        ram.usedGb  = static_cast<double>(usedKb) / kKbPerGb;
        ram.percent = static_cast<float>(100.0 * static_cast<double>(usedKb) / static_cast<double>(totalKb));
    }
    return ram;
}

// ---- Uptime ----------------------------------------------------------------

std::string readUptime() {
    struct sysinfo info;
    if (sysinfo(&info) != 0) return "N/A";

    const long days  = info.uptime / 86400;
    const long hours = (info.uptime % 86400) / 3600;
    const long mins  = (info.uptime % 3600) / 60;

    std::string text;
    if (days > 0)               text += format("%ldd ", days);
    if (hours > 0 || days > 0)  text += format("%ldh ", hours);
    text += format("%ldm", mins);
    return text;
}

// ---- GPU: PCI name lookup --------------------------------------------------

// Turns PCI ids (e.g. "0x1002", "0x73bf") into a friendly name using the
// pci.ids database that ships with every distro. "" if not found.
std::string lookupPciDeviceName(const std::string& vendorId, const std::string& deviceId) {
    static const char* const kPciIdsPaths[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"};

    if (vendorId.size() < 3 || deviceId.size() < 3) return "";
    const std::string vendor = vendorId.substr(2);   // drop the "0x"
    const std::string device = deviceId.substr(2);

    // File layout:   "1002  Vendor name"      <- no indent
    //                "\t73bf  Device name"    <- one tab
    std::string name;
    for (const char* path : kPciIdsPaths) {
        bool inVendorSection = false;
        forEachLine(path, [&](const char* line) {
            if (line[0] == '#' || line[0] == '\0') return kKeepGoing;
            if (line[0] != '\t') {
                if (inVendorSection) return kStop;   // reached the next vendor without finding the device
                inVendorSection = startsWith(line, vendor) && line[vendor.size()] == ' ';
                return kKeepGoing;
            }
            if (inVendorSection && line[1] != '\t' && startsWith(line + 1, device)) {
                name = trim(line + 1 + device.size());
                return kStop;
            }
            return kKeepGoing;
        });
        if (!name.empty()) break;
    }

    // "Navi 21 [Radeon RX 6800 / 6800 XT]" -> "Radeon RX 6800 / 6800 XT"
    const size_t open = name.find('[');
    const size_t close = name.rfind(']');
    if (open != std::string::npos && close != std::string::npos && close > open + 1) {
        name = name.substr(open + 1, close - open - 1);
    }
    return name;
}

// ---- GPU: NVIDIA -----------------------------------------------------------

// Runs ONE long-lived `nvidia-smi --loop-ms` process and reads its output.
// Starting nvidia-smi every refresh is slow (it can freeze the window for a
// moment and wastes CPU); a single process that streams a line per GPU at our
// update interval avoids that completely. If the user picks a different
// interval, start() is simply called again to replace the process.
class NvidiaSmiStream {
public:
    NvidiaSmiStream() = default;
    NvidiaSmiStream(const NvidiaSmiStream&) = delete;
    NvidiaSmiStream& operator=(const NvidiaSmiStream&) = delete;
    ~NvidiaSmiStream() { stop(); }

    bool running() const { return fd_ >= 0; }

    // Starts (or restarts) nvidia-smi so it prints a line per GPU every
    // `intervalMs`. Doesn't wait for the first lines, and the GPU numbers
    // appear at the next refresh.
    bool start(int intervalMs) {
        stop();
        buffer_.clear();
        const std::string program = findCommand("nvidia-smi");
        if (program.empty()) return false;   // nvidia-smi isn't installed

        int pipeEnds[2];
        if (pipe2(pipeEnds, O_CLOEXEC) != 0) return false;   // CLOEXEC: other programs started meanwhile mustn't inherit it

        // Child process: stdout -> our pipe, stderr -> /dev/null. In a process
        // group of its own, so stopping it also stops anything it started (it
        // may be a wrapper script).
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);

        const std::string loopArgument = "--loop-ms=" + std::to_string(intervalMs);
        const char* const args[] = {
            "nvidia-smi",   // every NVIDIA GPU, one line each
            "--query-gpu=index,gpu_name,utilization.gpu,memory.used,memory.total,clocks.current.graphics,temperature.gpu",
            "--format=csv,noheader,nounits",
            loopArgument.c_str(),
            nullptr};
        const int result = posix_spawn(&pid_, program.c_str(), &actions, &attributes, const_cast<char* const*>(args), environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pipeEnds[1]);

        if (result != 0) {
            close(pipeEnds[0]);
            pid_ = -1;
            return false;
        }
        fd_ = pipeEnds[0];
        setNonBlocking(fd_);   // reading must never wait for nvidia-smi
        return true;
    }

    // Returns the newest complete lines printed since the last call (none if
    // nothing new). Bounded however much it prints (a broken or fake nvidia-smi
    // can print without end, or without newlines): at most kMaxReadBytes read
    // per call, at most kMaxLineBytes kept of an unfinished line, and only the
    // newest kMaxLines lines returned.
    std::vector<std::string> newLines() {
        constexpr size_t kMaxReadBytes = 64 * 1024, kMaxLineBytes = 4096, kMaxLines = 64;
        std::vector<std::string> lines;
        if (fd_ < 0) return lines;

        char chunk[4096];
        size_t total = 0;
        ssize_t bytes = -1;
        while (total < kMaxReadBytes && (bytes = read(fd_, chunk, sizeof(chunk))) > 0) {
            buffer_.append(chunk, static_cast<size_t>(bytes));
            total += static_cast<size_t>(bytes);
        }
        if (bytes == 0) stop();   // nvidia-smi exited

        size_t lineStart = 0, newline;
        while ((newline = buffer_.find('\n', lineStart)) != std::string::npos) {
            if (newline - lineStart <= kMaxLineBytes) lines.emplace_back(buffer_, lineStart, newline - lineStart);
            lineStart = newline + 1;
        }
        buffer_.erase(0, lineStart);
        if (buffer_.size() > kMaxLineBytes) buffer_.clear();   // no line is this long: it's garbage
        if (lines.size() > kMaxLines) lines.erase(lines.begin(), lines.end() - kMaxLines);
        return lines;
    }

private:
    void stop() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            endChildProcess(pid_, true);   // never waits long (see there)
            pid_ = -1;
        }
    }

    pid_t pid_{-1};
    int fd_{-1};
    std::string buffer_;   // the start of a line that hasn't finished arriving
};

// Parses one line like "0, NVIDIA GeForce RTX 3080, 12, 1500, 10240, 1800, 64"
// (index, name, GPU %, VRAM used MiB, VRAM total MiB, clock MHz, temperature
// °C) into `gpu`. Each field stands alone: one nvidia-smi can't report
// ("[N/A]", "[Not Supported]") is just left out, and so is one out of range.
// Returns false if the line isn't a GPU line.
bool parseNvidiaLine(const std::string& line, int& index, GpuStats& gpu) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t comma = line.find(',', start);
        fields.push_back(trim(line.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    double number = 0.0;
    constexpr int kMaxGpus = 64;
    if (fields.size() < 6 || !parseNumber(fields[0], number) || number < 0.0 || number >= kMaxGpus) return false;
    index = static_cast<int>(number);

    gpu = GpuStats();
    gpu.vendor = GpuVendor::Nvidia;
    if (!fields[1].empty() && fields[1][0] != '[') gpu.name = fields[1];
    double usage = 0.0, usedMb = 0.0, totalMb = 0.0, clockMhz = 0.0;
    if (parseNumber(fields[2], usage) && usage >= 0.0 && usage <= 100.0) {
        gpu.gpuUsage = static_cast<float>(usage);
        gpu.hasLoad = true;
    }
    if (parseNumber(fields[3], usedMb) && parseNumber(fields[4], totalMb) && totalMb > 0.0 && usedMb >= 0.0 && usedMb <= totalMb) {
        gpu.hasMemory = true;
        gpu.vramUsedGb  = usedMb / 1024.0;
        gpu.vramTotalGb = totalMb / 1024.0;
        gpu.vramUsage   = static_cast<float>(100.0 * usedMb / totalMb);
    }
    if (parseNumber(fields[5], clockMhz) && clockMhz >= 0.0) gpu.clockGhz = clockMhz / 1000.0;
    double celsius = 0.0;
    if (fields.size() > 6 && parseNumber(fields[6], celsius) && isRealTemperature(celsius)) {
        gpu.temperatureC = celsius;
    }
    return true;
}

// ---- GPU: helpers shared by the AMD / Intel / Mali readers -------------------

// Shown instead of a load the GPU's driver doesn't report.
constexpr const char* kLoadNotReported = "Core load: not reported by driver";

// Every GPU appears as a folder like "/sys/class/drm/card0". This lists them,
// skipping display outputs ("card0-DP-1") and render nodes ("renderD128").
// `sysRoot` is "/sys" on a real machine; it is a parameter only so the code can
// be tested against a fake folder tree.
std::vector<std::string> listGpuCardDirs(const std::string& sysRoot) {
    std::vector<std::string> cards;
    for (const fs::path& entry : listDirectory(sysRoot + "/class/drm")) {
        const std::string node = entry.filename().string();
        if (startsWith(node, "card") && node.find('-') == std::string::npos) cards.push_back(entry.string());
    }
    return cards;
}

// The GPU's voltage sensor file ("in0_input", millivolts), or "" if it has none.
std::string findHwmonVoltageInput(const std::string& deviceDir) {
    for (const fs::path& hwmon : listDirectory(deviceDir + "/hwmon")) {
        const std::string input = hwmon.string() + "/in0_input";
        if (fileExists(input)) return input;
    }
    return "";
}

// The GPU's temperature sensor file, or "" if it has none. AMD cards report
// "edge" (the chip's surface, which is what most tools show), "junction" (its
// hottest spot) and "mem"; others have a single input.
std::string findHwmonTemperatureInput(const std::string& deviceDir) {
    for (const fs::path& hwmon : listDirectory(deviceDir + "/hwmon")) {
        std::string first;
        for (int i = 1; i <= 8; ++i) {
            const std::string input = format("%s/temp%d_input", hwmon.string().c_str(), i);
            if (!fileExists(input)) continue;
            if (readFirstLine(format("%s/temp%d_label", hwmon.string().c_str(), i)) == "edge") return input;
            if (first.empty()) first = input;
        }
        if (!first.empty()) return first;
    }
    return "";
}

// Reads every card with `readOne`, e.g. readAll(amdCards, readAmdCard).
template <typename Card, typename Reader>
std::vector<GpuStats> readAll(std::vector<Card>& cards, Reader readOne) {
    std::vector<GpuStats> all;
    for (Card& card : cards) all.push_back(readOne(card));
    return all;
}

// With several GPUs of one kind (say a laptop's integrated + dedicated card)
// show the busiest one, or the one with more VRAM if they are equally busy.
GpuStats pickBusiest(const std::vector<GpuStats>& candidates) {
    GpuStats best;
    bool first = true;
    for (const GpuStats& current : candidates) {
        if (first || current.gpuUsage > best.gpuUsage ||
            (current.gpuUsage == best.gpuUsage && current.vramTotalGb > best.vramTotalGb)) {
            best = current;
        }
        first = false;
    }
    return best;
}

// ---- GPU: AMD --------------------------------------------------------------

// Paths for one AMD GPU, worked out once at startup.
struct AmdCard {
    std::string name{"AMD Radeon GPU"};
    double vramTotalGb{0.0};
    std::string busyPath, vramUsedPath, clockPath, voltagePath, temperaturePath;
};

std::vector<AmdCard> findAmdCards(const std::string& sysRoot) {
    std::vector<AmdCard> cards;
    for (const std::string& cardDir : listGpuCardDirs(sysRoot)) {
        const std::string dir = cardDir + "/device";
        if (readFirstLine(dir + "/vendor") != kAmdVendorId) continue;

        AmdCard card;
        card.busyPath     = dir + "/gpu_busy_percent";
        card.vramUsedPath = dir + "/mem_info_vram_used";
        card.clockPath    = dir + "/pp_dpm_sclk";
        card.voltagePath  = findHwmonVoltageInput(dir);
        card.temperaturePath = findHwmonTemperatureInput(dir);

        std::string name = readFirstLine(dir + "/product_name");
        if (name.empty()) name = lookupPciDeviceName(kAmdVendorId, readFirstLine(dir + "/device"));
        if (!name.empty()) card.name = name;

        double vramBytes = 0.0;
        if (readNumber(dir + "/mem_info_vram_total", vramBytes)) card.vramTotalGb = vramBytes / kBytesPerGb;

        cards.push_back(card);
    }
    return cards;
}

GpuStats readAmdCard(const AmdCard& card) {
    GpuStats gpu;
    gpu.vendor = GpuVendor::Amd;
    gpu.name = card.name;
    gpu.vramTotalGb = card.vramTotalGb;

    double value = 0.0;
    if (readNumber(card.busyPath, value)) {
        gpu.gpuUsage = static_cast<float>(value);
        gpu.hasLoad = true;
    } else {
        gpu.loadNote = kLoadNotReported;
    }

    if (card.vramTotalGb > 0.0 && readNumber(card.vramUsedPath, value)) {
        gpu.vramUsedGb = value / kBytesPerGb;
        gpu.vramUsage  = static_cast<float>(100.0 * gpu.vramUsedGb / gpu.vramTotalGb);
        gpu.hasMemory = true;
    }

    // pp_dpm_sclk looks like "0: 300Mhz\n1: 1500Mhz *\n"; the line with '*' is the current clock.
    forEachLine(card.clockPath, [&](const char* line) {
        const char* colon = strchr(line, ':');
        if (strchr(line, '*') && colon) gpu.clockGhz = strtod(colon + 1, nullptr) / 1000.0;
        return kKeepGoing;
    });

    gpu.voltageV = readVoltage(card.voltagePath);
    gpu.temperatureC = readTemperature(card.temperaturePath);
    return gpu;
}

// ---- GPU: Intel ------------------------------------------------------------
//
// Intel GPUs have no "busy %" file that ordinary users can read. What they do
// report is how long the GPU has spent asleep (the "RC6" power-saving state),
// so the load shown is ESTIMATED as "the share of time NOT asleep". It runs a
// little high (awake but barely working still counts as busy), which is why the
// UI labels it "(est.)".
//
// Two kernel drivers exist - "i915" (most Intel GPUs) and "xe" (newest) - and
// they use different file names, so we look for the known files of both.

struct IntelCard {
    std::string name{"Intel Graphics"};
    std::string memoryNote;
    std::vector<std::string> clockPaths;   // current clock in MHz, best source first
    std::string idleCounterPath;           // total milliseconds spent asleep so far
    std::string voltagePath;
    std::string temperaturePath;           // graphics cards only: integrated GPUs share the CPU's

    // The previous reading, needed to turn the running total into a percentage.
    double lastIdleMs{0.0};
    double lastSampleMs{-1.0};   // -1 = no reading yet
    bool haveLoad{false};
    float loadPercent{0.0f};
};

// Integrated Intel graphics always sit at PCI address 0000:00:02.0 (Intel Arc
// graphics cards do not). Integrated GPUs use ordinary system RAM as video memory.
bool isIntegratedIntelGpu(const std::string& deviceDir) {
    std::error_code error;
    const fs::path realPath = fs::canonical(deviceDir, error);
    return !error && realPath.filename() == "0000:00:02.0";
}

std::vector<IntelCard> findIntelCards(const std::string& sysRoot) {
    std::vector<IntelCard> cards;
    for (const std::string& cardDir : listGpuCardDirs(sysRoot)) {
        const std::string dir = cardDir + "/device";
        if (readFirstLine(dir + "/vendor") != kIntelVendorId) continue;

        IntelCard card;
        const std::string name = lookupPciDeviceName(kIntelVendorId, readFirstLine(dir + "/device"));
        if (!name.empty()) card.name = name;
        card.memoryNote = isIntegratedIntelGpu(dir) ? "VRAM: shares system RAM" : "VRAM: not reported by driver";

        card.clockPaths = existingPaths({
            cardDir + "/gt/gt0/rps_act_freq_mhz", cardDir + "/gt_act_freq_mhz",    // i915: actual clock
            cardDir + "/gt/gt0/rps_cur_freq_mhz", cardDir + "/gt_cur_freq_mhz",    // i915: requested clock
            dir + "/tile0/gt0/freq0/act_freq",    dir + "/tile0/gt0/freq0/cur_freq"});   // xe
        card.idleCounterPath = firstExisting({
            cardDir + "/gt/gt0/rc6_residency_ms", cardDir + "/power/rc6_residency_ms",   // i915
            dir + "/tile0/gt0/gtidle/idle_residency_ms"});                               // xe
        card.voltagePath = findHwmonVoltageInput(dir);
        card.temperaturePath = findHwmonTemperatureInput(dir);

        cards.push_back(card);
    }
    return cards;
}

GpuStats readIntelCard(IntelCard& card) {
    GpuStats gpu;
    gpu.vendor = GpuVendor::Intel;
    gpu.name = card.name;
    gpu.memoryNote = card.memoryNote;
    gpu.voltageV = readVoltage(card.voltagePath);
    gpu.temperatureC = readTemperature(card.temperaturePath);

    for (const std::string& path : card.clockPaths) {   // a reading of 0 means "asleep right now", so try the next file
        double mhz = 0.0;
        if (readNumber(path, mhz) && mhz > 0.0) {
            gpu.clockGhz = mhz / 1000.0;
            break;
        }
    }

    double idleMs = 0.0;
    if (card.idleCounterPath.empty() || !readNumber(card.idleCounterPath, idleMs)) {
        gpu.loadNote = kLoadNotReported;
        return gpu;
    }

    const double nowMs = monotonicMs();
    if (card.lastSampleMs < 0.0) {   // first reading: nothing to compare with yet
        card.lastIdleMs = idleMs;
        card.lastSampleMs = nowMs;
    } else if (nowMs - card.lastSampleMs >= 100.0) {
        const double asleepFraction = (idleMs - card.lastIdleMs) / (nowMs - card.lastSampleMs);
        card.loadPercent = static_cast<float>(100.0 * std::clamp(1.0 - asleepFraction, 0.0, 1.0));
        card.haveLoad = true;
        card.lastIdleMs = idleMs;
        card.lastSampleMs = nowMs;
    }

    if (card.haveLoad) {
        gpu.gpuUsage = card.loadPercent;
        gpu.hasLoad = true;
        gpu.loadIsEstimate = true;
    } else {
        gpu.loadNote = "Core load: measuring...";
    }
    return gpu;
}

// ---- GPU: ARM Mali ---------------------------------------------------------
//
// Mali GPUs (found in many handhelds and single-board computers) are not on
// the PCI bus. The kernel's "devfreq" system reports the GPU's current clock,
// and some vendor kernels also report a load percentage. The standard open
// drivers (panfrost, panthor, lima) only give the clock, so for those the
// load bar is replaced by a short note.

struct MaliGpu {
    std::string name{"ARM Mali GPU"};
    std::string clockPath;   // devfreq "cur_freq", in Hz
    std::string loadPath;    // a file holding the load percent, or "" if there is none
    std::string temperaturePath;   // a thermal zone, or ""
};

// True if the device folder belongs to a Mali GPU (by driver name or device tree).
bool isMaliDevice(const std::string& deviceDir) {
    static const char* const kMaliDrivers[] = {"panfrost", "panthor", "lima", "mali", "mali_kbase"};

    std::error_code error;
    const std::string driver = fs::read_symlink(deviceDir + "/driver", error).filename().string();
    for (const char* known : kMaliDrivers) {
        if (driver == known) return true;
    }
    for (const std::string& compatible : readNulSeparatedStrings(deviceDir + "/of_node/compatible")) {
        if (startsWith(compatible, "arm,mali")) return true;
    }
    return false;
}

// "arm,mali-bifrost" in the device tree -> "ARM Mali (Bifrost)".
std::string maliModelName(const std::string& deviceDir) {
    for (std::string model : readNulSeparatedStrings(deviceDir + "/of_node/compatible")) {
        if (!startsWith(model, "arm,mali-") || model.size() <= strlen("arm,mali-")) continue;
        model = model.substr(strlen("arm,mali-"));
        model[0] = static_cast<char>(toupper(static_cast<unsigned char>(model[0])));
        return "ARM Mali (" + model + ")";
    }
    return "ARM Mali GPU";
}

std::optional<MaliGpu> findMaliGpu(const std::string& sysRoot) {
    // Open drivers register a normal GPU card; Mali's own "kbase" driver
    // registers /sys/class/misc/mali0 instead.
    std::string deviceDir;
    for (const std::string& cardDir : listGpuCardDirs(sysRoot)) {
        if (isMaliDevice(cardDir + "/device")) {
            deviceDir = cardDir + "/device";
            break;
        }
    }
    if (deviceDir.empty() && fileExists(sysRoot + "/class/misc/mali0/device")) {
        deviceDir = sysRoot + "/class/misc/mali0/device";
    }
    if (deviceDir.empty()) return std::nullopt;

    MaliGpu gpu;
    gpu.name = maliModelName(deviceDir);

    // Different kernels put the load percent in different places.
    std::vector<std::string> loadCandidates = {deviceDir + "/utilisation", deviceDir + "/utilization"};
    const std::vector<fs::path> devfreqFolders = listDirectory(deviceDir + "/devfreq");
    if (!devfreqFolders.empty()) {
        const std::string devfreqDir = devfreqFolders.front().string();
        gpu.clockPath = devfreqDir + "/cur_freq";
        loadCandidates.insert(loadCandidates.begin(), devfreqDir + "/load");
    }
    gpu.loadPath = firstExisting(loadCandidates);
    gpu.temperaturePath = findThermalZone(sysRoot, {"gpu"});
    return gpu;
}

GpuStats readMaliGpu(const MaliGpu& mali) {
    GpuStats gpu;
    gpu.vendor = GpuVendor::Mali;
    gpu.name = mali.name;
    gpu.memoryNote = "VRAM: shares system RAM";
    gpu.temperatureC = readTemperature(mali.temperaturePath);

    double value = 0.0;
    if (!mali.clockPath.empty() && readNumber(mali.clockPath, value)) gpu.clockGhz = value / 1e9;   // devfreq reports Hz

    // "load" files look like "23@800000000Hz" (load, then clock) and "utilisation"
    // files are a plain "23". Reading the number at the start handles both.
    if (!mali.loadPath.empty() && readNumber(mali.loadPath, value) && value >= 0.0 && value <= 100.0) {
        gpu.gpuUsage = static_cast<float>(value);
        gpu.hasLoad = true;
    } else {
        gpu.loadNote = kLoadNotReported;
    }
    return gpu;
}

// ---- GPU: pick the right reader and give the UI one simple interface --------

// True if an NVIDIA graphics card is plugged in. Checked on the PCI bus rather
// than in /sys/class/drm, because the proprietary driver only appears there
// with kernel modesetting on.
bool hasNvidiaGpu(const std::string& sysRoot) {
    for (const fs::path& device : listDirectory(sysRoot + "/bus/pci/devices")) {
        const std::string dir = device.string();
        if (readFirstLine(dir + "/vendor") == kNvidiaVendorId && startsWith(readFirstLine(dir + "/class"), "0x03")) return true;   // 0x03 = display controller
    }
    return false;
}

class GpuMonitor {
public:
    // `refreshMs` is the update interval, which NVIDIA's nvidia-smi helper is started at.
    explicit GpuMonitor(int refreshMs, const std::string& sysRoot = "/sys") : nvidiaIntervalMs_(refreshMs) {
        amdCards_   = findAmdCards(sysRoot);
        intelCards_ = findIntelCards(sysRoot);
        mali_       = findMaliGpu(sysRoot);

        // If a machine has several kinds of GPU (typical for laptops) the most
        // powerful one is shown: NVIDIA, then AMD, then Intel, then Mali. NVIDIA
        // only counts if a card is actually plugged in: nvidia-smi can be left
        // installed after the card is swapped for another brand.
        const bool nvidiaGpu = hasNvidiaGpu(sysRoot);
        if (nvidiaGpu && (fileExists("/proc/driver/nvidia/gpus") || commandExists("nvidia-smi"))) vendor_ = GpuVendor::Nvidia;
        else if (!amdCards_.empty())   vendor_ = GpuVendor::Amd;
        else if (!intelCards_.empty()) vendor_ = GpuVendor::Intel;
        else if (nvidiaGpu)            vendor_ = GpuVendor::Nvidia;   // open "nouveau" driver: no stats, but we know the brand
        else if (mali_)                vendor_ = GpuVendor::Mali;

        if (vendor_ == GpuVendor::Nvidia) startNvidiaSmi();
    }

    // Called when the user picks a new update interval. Only NVIDIA needs to
    // react: its nvidia-smi helper is restarted at the new rate. The other
    // GPUs are read directly from files each refresh.
    void setRefreshIntervalMs(int intervalMs) {
        if (vendor_ != GpuVendor::Nvidia || intervalMs == nvidiaIntervalMs_) return;
        nvidiaIntervalMs_ = intervalMs;
        startNvidiaSmi();
    }

    GpuStats read() {
        switch (vendor_) {
            case GpuVendor::Nvidia:  return readNvidia();
            case GpuVendor::Amd:     return pickBusiest(readAll(amdCards_, readAmdCard));
            case GpuVendor::Intel:   return pickBusiest(readAll(intelCards_, readIntelCard));
            case GpuVendor::Mali:    return readMaliGpu(*mali_);
            case GpuVendor::Unknown: break;
        }
        return GpuStats();
    }

private:
    static constexpr double kNvidiaRetryMs = 10000.0;   // how often to try restarting a stopped nvidia-smi

    void startNvidiaSmi() {
        lastNvidiaStartMs_ = monotonicMs();
        nvidia_.start(nvidiaIntervalMs_);
    }

    // nvidia-smi prints a line per GPU each interval; the newest reading of each
    // GPU is kept, and the busiest is shown (as for AMD and Intel).
    GpuStats readNvidia() {
        const double now = monotonicMs();
        for (const std::string& line : nvidia_.newLines()) {
            int index = 0;
            GpuStats gpu;
            if (parseNvidiaLine(line, index, gpu)) nvidiaCards_[index] = {gpu, now};
        }
        // A card nvidia-smi stopped listing (an eGPU unplugged, a card that fell
        // off the bus) is dropped after a few missed updates, not shown frozen.
        const double staleMs = std::max(3.0 * nvidiaIntervalMs_, 3000.0);
        for (auto card = nvidiaCards_.begin(); card != nvidiaCards_.end();) {
            card = (now - card->second.seenMs > staleMs) ? nvidiaCards_.erase(card) : std::next(card);
        }
        if (!nvidia_.running()) {
            // nvidia-smi isn't installed, or has stopped (after a driver hiccup,
            // say). Show the GPU as unavailable rather than its last numbers,
            // and try starting it again every so often.
            nvidiaCards_.clear();
            if (monotonicMs() - lastNvidiaStartMs_ >= kNvidiaRetryMs) startNvidiaSmi();
        }
        if (nvidiaCards_.empty()) {
            GpuStats unavailable;
            unavailable.vendor = GpuVendor::Nvidia;
            unavailable.name = nvidiaName_;
            return unavailable;
        }
        std::vector<GpuStats> cards;
        for (const auto& [index, card] : nvidiaCards_) cards.push_back(card.stats);
        const GpuStats busiest = pickBusiest(cards);
        if (busiest.name != GpuStats().name) nvidiaName_ = busiest.name;   // remembered for while it's unavailable
        return busiest;
    }

    GpuVendor vendor_{GpuVendor::Unknown};
    int nvidiaIntervalMs_;
    NvidiaSmiStream nvidia_;
    double lastNvidiaStartMs_{0.0};
    struct NvidiaCard {
        GpuStats stats;
        double seenMs{0.0};   // when nvidia-smi last listed it (monotonicMs)
    };
    std::map<int, NvidiaCard> nvidiaCards_;   // newest reading per GPU index
    std::string nvidiaName_{GpuStats().name};
    std::vector<AmdCard> amdCards_;
    std::vector<IntelCard> intelCards_;
    std::optional<MaliGpu> mali_;
};

// ---- Everything live, in one place -----------------------------------------

class LiveMonitor {
public:
    // `refreshMs` is the update interval (see GpuMonitor).
    explicit LiveMonitor(int refreshMs)
        : previousCpu_(readCpuTimes()), cpuVoltageInputs_(findCpuVoltageInputs()),
          cpuTemperatureInput_(findCpuTemperatureInput()), gpu_(refreshMs) {
        stats_.cpuPercent.assign(previousCpu_.size(), 0.0f);
        for (const CpuTimes& core : previousCpu_) stats_.cpuIds.push_back(core.id);
        readSensors();   // everything except CPU load, which needs two samples
    }

    void refresh() {
        updateCpuLoad();
        readSensors();
    }

    void setRefreshIntervalMs(int intervalMs) { gpu_.setRefreshIntervalMs(intervalMs); }

    const LiveStats& stats() const { return stats_; }

private:
    // CPU load = how much of the time since the last sample each core spent working.
    // Cores are matched by number, not position: one going offline and another
    // coming back in the same interval keeps the count but shifts the rest.
    void updateCpuLoad() {
        std::vector<CpuTimes> now = readCpuTimes();
        std::vector<float> percent(now.size(), 0.0f);   // 0 for a core that just came online
        std::vector<int> ids(now.size());
        // The counters only ever go up, except iowait, which the kernel may lower:
        // each is compared on its own, and one that went down counts as no time.
        const auto grown = [](unsigned long long current, unsigned long long before) {
            return current > before ? static_cast<double>(current - before) : 0.0;
        };
        for (size_t i = 0; i < now.size(); ++i) {
            ids[i] = now[i].id;
            size_t before = i;   // usually the same position
            if (before >= previousCpu_.size() || previousCpu_[before].id != now[i].id) {
                before = std::find_if(previousCpu_.begin(), previousCpu_.end(), [&](const CpuTimes& core) { return core.id == now[i].id; }) -
                         previousCpu_.begin();
                if (before == previousCpu_.size()) continue;
            }
            const CpuTimes& was = previousCpu_[before];
            const double active = grown(now[i].active, was.active);
            const double total = active + grown(now[i].idle, was.idle) + grown(now[i].iowait, was.iowait);
            if (total > 0.0) percent[i] = static_cast<float>(std::clamp(100.0 * active / total, 0.0, 100.0));
            else if (before < stats_.cpuPercent.size()) percent[i] = stats_.cpuPercent[before];   // no time passed: keep the last value
        }
        stats_.cpuPercent = std::move(percent);
        stats_.cpuIds = std::move(ids);
        previousCpu_ = std::move(now);
    }

    void readSensors() {
        stats_.ram        = readRamStats();
        stats_.gpu        = gpu_.read();
        stats_.cpuFreqGhz = readCpuFreqGhz();
        stats_.cpuTemperatureC = readTemperature(cpuTemperatureInput_);
        stats_.uptime     = readUptime();

        const double now = monotonicMs();
        if (lastVoltageReadMs_ < 0.0 || now - lastVoltageReadMs_ >= kVoltageReadIntervalMs) {
            stats_.cpuVoltage = readCpuVoltage(cpuVoltageInputs_);
            lastVoltageReadMs_ = now;
        }
    }

    std::vector<CpuTimes> previousCpu_;
    std::vector<std::string> cpuVoltageInputs_;
    double lastVoltageReadMs_{-1.0};   // -1 = not read yet
    std::string cpuTemperatureInput_;
    GpuMonitor gpu_;
    LiveStats stats_;
};

// ---- Storage usage -------------------------------------------------------------
// Read when the storage menu is open, not every refresh.

// /proc/mounts writes spaces and tabs in paths as octal escapes ("My\040Drive").
std::string unescapeMountPath(std::string_view text) {
    const auto isOctal = [&](size_t at) { return at < text.size() && text[at] >= '0' && text[at] <= '7'; };
    std::string path;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && isOctal(i + 1) && isOctal(i + 2) && isOctal(i + 3)) {
            path += static_cast<char>((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 + (text[i + 3] - '0'));
            i += 3;
        } else {
            path += text[i];
        }
    }
    return path;
}

// The block device ("nvme0n1p2", "dm-0") with device number `device`, or "" if
// there's none (filesystems that aren't on a disk get numbers of their own).
std::string blockDeviceName(dev_t device) {
    std::error_code error;
    const fs::path path = fs::canonical(format("/sys/dev/block/%u:%u", major(device), minor(device)), error);
    return error ? "" : path.filename().string();
}

// The disks ("nvme0n1") that a block device ("nvme0n1p2", "dm-0", "md0") is on.
// A partition's sysfs folder sits inside its disk's folder; an encrypted, LVM
// or RAID volume lists the devices it is built on under "slaves", which can be
// on several disks (each of them then lists the volume's mount points).
std::vector<std::string> disksOfBlockDevice(const std::string& name, int depth = 0) {
    const std::string dir = "/sys/class/block/" + name;
    if (fileExists(dir + "/partition")) {
        std::error_code error;
        return {fs::canonical(dir, error).parent_path().filename().string()};
    }
    std::vector<std::string> disks;
    if (depth < 4) {
        for (const fs::path& slave : listDirectory(dir + "/slaves")) {
            for (std::string& disk : disksOfBlockDevice(slave.filename().string(), depth + 1)) {
                if (std::find(disks.begin(), disks.end(), disk) == disks.end()) disks.push_back(std::move(disk));
            }
        }
    }
    if (disks.empty()) disks.push_back(name);
    return disks;
}

// The block device a mounted filesystem is really on, going by device numbers
// rather than by the name in the mount table, which whoever mounted it chose
// (a FUSE filesystem can call itself "/dev/nvme0n1p1"). "" if it isn't on one.
std::string mountedBlockDevice(const std::string& source, const std::string& mountPoint, const std::string& type) {
    struct stat mounted;
    if (stat(mountPoint.c_str(), &mounted) != 0) return "";
    struct stat device;
    if (stat(source.c_str(), &device) != 0 || !S_ISBLK(device.st_mode)) {
        return blockDeviceName(mounted.st_dev);   // "/dev/root" and the like: the mount's own number tells
    }
    if (mounted.st_dev == device.st_rdev) return blockDeviceName(device.st_rdev);
    // btrfs gives every mount a number of its own, so there only the listed
    // device can be gone by (mounting btrfs takes root anyway).
    return type == "btrfs" ? blockDeviceName(device.st_rdev) : "";
}

// Every storage device with how much of it is used and free. Can wait a long
// time on a filesystem that has stopped answering, so it's only ever called
// on a helper thread (see StorageReader).
std::vector<DiskUsage> readDiskUsage() {
    std::vector<DiskUsage> disks;
    for (const DriveInfo& drive : readStorageDrives()) {
        DiskUsage usage;
        usage.drive = drive;
        disks.push_back(usage);
    }

    // Each filesystem once, at its shortest mount point: btrfs mounts the same
    // partition several times (/, /home, /var/log, ...).
    std::map<std::string, std::string> mountPointOf;   // "sda2" -> "/"
    forEachLine("/proc/self/mounts", [&](const char* line) {
        // "source mount-point type options ...", separated by single spaces
        // (spaces inside a field are written as \040).
        std::string_view rest(line), fields[3];
        for (std::string_view& field : fields) {
            const size_t space = rest.find(' ');
            field = rest.substr(0, space);
            rest.remove_prefix(space == std::string_view::npos ? rest.size() : space + 1);
        }
        const std::string_view source = fields[0], type = fields[2];
        // Only filesystems on a device; FUSE ones other than "fuseblk" (NTFS,
        // exFAT) aren't, and one that has stopped answering would hold the read up.
        if (!startsWith(source, "/dev/") || type == "fuse" || startsWith(type, "fuse.")) return kKeepGoing;
        const std::string path = unescapeMountPath(fields[1]);
        const std::string device = mountedBlockDevice(unescapeMountPath(source), path, std::string(type));
        if (device.empty()) return kKeepGoing;
        const auto [entry, added] = mountPointOf.emplace(device, path);
        if (!added && path.size() < entry->second.size()) entry->second = path;
        return kKeepGoing;
    });

    for (const auto& [device, mountPoint] : mountPointOf) {
        struct statvfs space;
        if (statvfs(mountPoint.c_str(), &space) != 0) continue;
        const double gbPerBlock = static_cast<double>(space.f_frsize) / kBytesPerGb;
        for (const std::string& diskName : disksOfBlockDevice(device)) {
            const auto disk = std::find_if(disks.begin(), disks.end(),
                                           [&](const DiskUsage& usage) { return usage.drive.deviceName == diskName; });
            if (disk == disks.end()) continue;
            disk->usedGb += static_cast<double>(space.f_blocks - space.f_bfree) * gbPerBlock;
            disk->freeGb += static_cast<double>(space.f_bavail) * gbPerBlock;
            disk->mountPoints.push_back(mountPoint);
        }
    }
    for (DiskUsage& disk : disks) std::sort(disk.mountPoints.begin(), disk.mountPoints.end());
    return disks;
}

// ============================================================================
// 5. UI
// ============================================================================

// ---- Settings --------------------------------------------------------------

enum class Skin { Classic, Jarv, Galactic, Gunship, Halloween, Poseidon, Otaku, Cyberpunk, Gnome };

// Every skin, in the order the Skin dropdown lists them. `id` is how
// settings.conf stores it; `name` is what the dropdown shows.
struct SkinChoice {
    Skin skin;
    const char* id;
    const char* name;
};
constexpr SkinChoice kSkinChoices[] = {
    {Skin::Classic,  "classic",  "Classic"},
    {Skin::Jarv,     "jarv",     "JARV"},
    {Skin::Galactic, "galactic", "Galactic Conflict"},
    {Skin::Gunship,  "gunship",  "Federation Gunship"},
    {Skin::Halloween, "halloween", "Halloween"},
    {Skin::Poseidon,  "poseidon",  "Poseidon"},
    {Skin::Otaku,     "otaku",     "Otaku"},
    {Skin::Cyberpunk, "cyberpunk", "Cyber-Punk"},
    {Skin::Gnome,     "gnome",     "Classroom"},
};

const SkinChoice& skinChoice(Skin skin) {
    for (const SkinChoice& choice : kSkinChoices) {
        if (choice.skin == skin) return choice;
    }
    return kSkinChoices[0];
}

// What the settings menu controls. Saved to ~/.config/veeastats/settings.conf
// whenever it changes, so the choice survives a restart.
struct Settings {
    Skin skin{Skin::Classic};
    bool animations{true};   // every skin but Classic: turning rings, blinking lights, gliding numbers
    int refreshMs{kDefaultRefreshMs};
    bool runInBackground{false};   // closing the window hides it, so the overlay hotkey keeps working
    bool limitOverlayToApp{true};  // the overlay opens only on a focused app, and lives and dies with it
    bool fahrenheit{false};        // temperatures in °F instead of °C
    bool borderless{false};        // the window has no title bar or border
};

bool operator!=(const Settings& a, const Settings& b) {
    return a.skin != b.skin || a.animations != b.animations || a.refreshMs != b.refreshMs ||
           a.runInBackground != b.runInBackground || a.limitOverlayToApp != b.limitOverlayToApp || a.fahrenheit != b.fahrenheit ||
           a.borderless != b.borderless;
}

std::string settingsFilePath() {
    const std::string configHome = xdgDirectory("XDG_CONFIG_HOME", ".config");
    return configHome.empty() ? "" : configHome + "/veeastats/settings.conf";
}

// A missing or unreadable file simply gives the defaults, and so does an
// interval that isn't one of the menu's choices.
Settings loadSettings() {
    Settings settings;
    forEachLine(settingsFilePath(), [&](const char* line) {
        const std::string text = trim(line);
        if (startsWith(text, "skin=")) {
            for (const SkinChoice& choice : kSkinChoices) {
                if (text.substr(strlen("skin=")) == choice.id) settings.skin = choice.skin;
            }
        }
        else if (text == "animations=0") settings.animations = false;
        else if (text == "background=1") settings.runInBackground = true;
        else if (text == "limit_overlay=0") settings.limitOverlayToApp = false;
        else if (text == "fahrenheit=1") settings.fahrenheit = true;
        else if (text == "borderless=1") settings.borderless = true;
        else if (startsWith(text, "refresh_ms=")) {
            const int ms = atoi(text.c_str() + strlen("refresh_ms="));
            if (std::find(std::begin(kRefreshChoicesMs), std::end(kRefreshChoicesMs), ms) != std::end(kRefreshChoicesMs)) {
                settings.refreshMs = ms;
            }
        }
        return kKeepGoing;
    });
    return settings;
}

void saveSettings(const Settings& settings) {
    const std::string path = settingsFilePath();
    if (path.empty()) return;
    std::error_code error;
    fs::create_directories(fs::path(path).parent_path(), error);
    FILE* file = fopen(path.c_str(), "w");
    if (!file) return;
    fprintf(file, "skin=%s\nanimations=%d\nrefresh_ms=%d\nbackground=%d\nlimit_overlay=%d\nfahrenheit=%d\nborderless=%d\n",
            skinChoice(settings.skin).id, settings.animations ? 1 : 0, settings.refreshMs,
            settings.runInBackground ? 1 : 0, settings.limitOverlayToApp ? 1 : 0, settings.fahrenheit ? 1 : 0,
            settings.borderless ? 1 : 0);
    fclose(file);
}

// The storage menu: its device list, and the device whose chart is showing.
// Wakes the main loop to draw a frame (section 8).
void wakeMainLoop();

// Reads storage usage on a helper thread: statvfs on a filesystem that has
// stopped answering (a USB drive pulled out mid-read, a stuck network mount)
// can wait forever, and must never take the window with it. One read at a
// time; while one is stuck, the last numbers stay up.
class StorageReader {
public:
    // Starts a read, unless one is still going.
    void request() {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->reading) return;
        shared_->reading = true;
        std::thread([shared = shared_] {   // the shared state outlives the reader if a read never ends
            std::vector<DiskUsage> disks = readDiskUsage();
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->result = std::move(disks);
                shared->ready = true;
                shared->reading = false;
            }
            wakeMainLoop();
        }).detach();
    }

    // Moves the newest finished read into `disks`. False if there's none new.
    bool take(std::vector<DiskUsage>& disks) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->ready) return false;
        disks = std::move(shared_->result);
        shared_->ready = false;
        return true;
    }

private:
    struct Shared {
        std::mutex mutex;
        bool reading{false};
        bool ready{false};
        std::vector<DiskUsage> result;
    };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

struct StorageMenu {
    bool open{false};
    std::string selected;            // deviceName of the chosen device ("" = none)
    std::vector<DiskUsage> disks;
    bool haveDisks{false};           // `disks` has been read at least once
    StorageReader reader;
    double readAt{-1.0e9};           // ImGui time the last read was asked for
};

// Everything the user can change from the window. main() reacts to changes after each frame.
struct UiState {
    bool settingsOpen{false};   // the gear's menu is showing
    StorageMenu storage;        // the storage button's menu
    bool quitRequested{false};  // "Quit VeeaStats" in the gear menu (only offered in background mode)
    Settings settings;
};

// ---- Shared widgets ----------------------------------------------------------

// Colours
const ImVec4 kGreen (0.20f, 0.75f, 0.35f, 1.0f);   // usage bars: normal / busy / critical
const ImVec4 kOrange(0.95f, 0.65f, 0.15f, 1.0f);
const ImVec4 kRed   (0.90f, 0.25f, 0.25f, 1.0f);

const ImVec4 kTitleColor  (0.90f, 0.90f, 0.90f, 1.0f);
const ImVec4 kCpuHeading  (0.40f, 0.80f, 1.00f, 1.0f);
const ImVec4 kRamHeading  (1.00f, 0.80f, 0.40f, 1.0f);
const ImVec4 kGpuHeading  (0.90f, 0.40f, 0.90f, 1.0f);
const ImVec4 kInfoHeading (0.20f, 0.85f, 0.60f, 1.0f);
const ImVec4 kClockColor  (0.20f, 0.85f, 0.40f, 1.0f);
const ImVec4 kVoltageColor(0.30f, 0.90f, 0.90f, 1.0f);
const ImVec4 kTemperatureColor(0.95f, 0.85f, 0.55f, 1.0f);   // below kHotC
const ImVec4 kErrorColor  (0.90f, 0.30f, 0.30f, 1.0f);

// Temperatures from here up are shown as hot (orange), then too hot (red).
constexpr double kHotC = 80.0, kTooHotC = 90.0;

const ImVec4& temperatureColor(double celsius) {
    return celsius >= kTooHotC ? kRed : celsius >= kHotC ? kOrange : kTemperatureColor;
}

// "62°C" or "144°F" from a reading in °C, or "N/A" without a sensor.
std::string temperatureText(double celsius, bool fahrenheit) {
    constexpr const char* kDegree = "\xC2\xB0";   // "°" in UTF-8
    if (!hasTemperature(celsius)) return "N/A";
    return fahrenheit ? format("%.0f%sF", celsius * 9.0 / 5.0 + 32.0, kDegree) : format("%.0f%sC", celsius, kDegree);
}

// Green, then orange above warnAt and red above critAt (fractions, 0..1).
const ImVec4& usageColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kRed : (fraction > warnAt) ? kOrange : kGreen;
}

// "label [=====----] text" - the bar turns orange above warnAt and red above critAt (0..1).
void drawUsageBar(const char* label, float percent, const std::string& text, float warnAt, float critAt) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, usageColor(percent, warnAt, critAt));
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::ProgressBar(percent / 100.0f, ImVec2(0.0f, 0.0f), text.c_str());
    ImGui::PopStyleColor();
}

// "3.20 GHz | 1.100 V | 62°C", or greyed-out "N/A" placeholders when a sensor is missing.
void drawSensorLine(double clockGhz, double volts, const char* voltageName, double celsius, bool fahrenheit) {
    if (clockGhz > 0.0) ImGui::TextColored(kClockColor, "%.2f GHz", clockGhz);
    else                ImGui::TextDisabled("Clock: N/A");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (volts > 0.0) ImGui::TextColored(kVoltageColor, "%.3f V", volts);
    else             ImGui::TextDisabled("%s: N/A", voltageName);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (hasTemperature(celsius)) ImGui::TextColored(temperatureColor(celsius), "%s", temperatureText(celsius, fahrenheit).c_str());
    else               ImGui::TextDisabled("Temp: N/A");
}

// A two-column "Property | Value" table. Returns false if it can't be shown;
// otherwise the caller adds rows with drawInfoRow() and then calls ImGui::EndTable().
bool beginInfoTable(const char* id, float labelWidth) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) return false;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void drawInfoRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void drawCpuSection(const StaticInfo& info, const LiveStats& live, bool fahrenheit) {
    ImGui::TextColored(kCpuHeading, "Per-Core CPU Load");
    ImGui::TextWrapped("%s", info.cpuModel.c_str());
    drawSensorLine(live.cpuFreqGhz, live.cpuVoltage, "VCore", live.cpuTemperatureC, fahrenheit);
    ImGui::Spacing();

    ImGui::BeginChild("CpuCoresRegion", ImVec2(0, 200), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (size_t i = 0; i < live.cpuPercent.size(); ++i) {
        drawUsageBar(format("CPU %2d", i < live.cpuIds.size() ? live.cpuIds[i] : static_cast<int>(i)).c_str(), live.cpuPercent[i], format("%.1f%%", live.cpuPercent[i]), 0.50f, 0.80f);
    }
    ImGui::EndChild();
}

void drawRamSection(const StaticInfo& info, const LiveStats& live) {
    ImGui::TextColored(kRamHeading, "System Memory");
    ImGui::Spacing();

    const RamStats& ram = live.ram;
    drawUsageBar("RAM ", ram.percent, format("%.2f / %.2f GB (%.1f%%)", ram.usedGb, ram.totalGb, ram.percent), 0.65f, 0.85f);

    if (info.ramModules.empty()) {
        ImGui::TextDisabled("Module Models: Standard System RAM");
        return;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Detected Memory Modules:");
    for (const std::string& module : info.ramModules) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", module.c_str());
    }
}

const char* vendorName(GpuVendor vendor) {
    switch (vendor) {
        case GpuVendor::Nvidia:  return "NVIDIA";
        case GpuVendor::Amd:     return "AMD Radeon";
        case GpuVendor::Intel:   return "Intel";
        case GpuVendor::Mali:    return "ARM Mali";
        case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

// " (AMD Radeon)" for the GPU heading, or "" if the vendor is unknown.
std::string vendorTag(GpuVendor vendor) {
    return vendor == GpuVendor::Unknown ? "" : format(" (%s)", vendorName(vendor));
}

void drawGpuSection(const LiveStats& live, bool fahrenheit) {
    const GpuStats& gpu = live.gpu;

    ImGui::TextColored(kGpuHeading, "GPU Metrics%s", vendorTag(gpu.vendor).c_str());
    ImGui::TextWrapped("Card: %s", gpu.name.c_str());
    drawSensorLine(gpu.clockGhz, gpu.voltageV, "VDDC", gpu.temperatureC, fahrenheit);
    ImGui::Spacing();

    if (!gpu.hasLoad && !gpu.hasMemory && gpu.loadNote.empty()) {
        ImGui::TextColored(kErrorColor, "GPU stats unavailable");
        return;
    }

    // Core load: a bar if the driver gives us a number, otherwise a short explanation.
    if (gpu.hasLoad) {
        drawUsageBar("Core", gpu.gpuUsage, format("%.1f%%%s", gpu.gpuUsage, gpu.loadIsEstimate ? " (est.)" : ""), 0.50f, 0.85f);
    } else if (!gpu.loadNote.empty()) {
        ImGui::TextDisabled("%s", gpu.loadNote.c_str());
    }

    // Video memory: a bar for cards with their own memory, otherwise a note
    // (integrated GPUs and Mali use ordinary system RAM).
    if (gpu.hasMemory) {
        drawUsageBar("VRAM", gpu.vramUsage,
                     format("%.2f / %.2f GB (%.1f%%)", gpu.vramUsedGb, gpu.vramTotalGb, gpu.vramUsage), 0.65f, 0.85f);
    } else if (!gpu.memoryNote.empty()) {
        ImGui::TextDisabled("%s", gpu.memoryNote.c_str());
    }
}

void drawHardwareInfo(const StaticInfo& info, const LiveStats& live) {
    ImGui::TextColored(kInfoHeading, "Hardware Information");
    ImGui::Spacing();
    if (!beginInfoTable("HardwareInfoTable", 110.0f)) return;

    drawInfoRow("CPU Model", info.cpuModel);
    drawInfoRow("GPU Model", live.gpu.name);
    drawInfoRow("RAM Size", format("%.2f GB", live.ram.totalGb));
    drawInfoRow("Motherboard", info.board.name);
    drawInfoRow("Chipset", info.board.chipset);
    for (size_t i = 0; i < info.drives.size(); ++i) {
        const DriveInfo& drive = info.drives[i];
        drawInfoRow(i == 0 ? "Storage Drives" : "",
                    format("[%s] %s - %.1f GB (%s)", drive.deviceName.c_str(), drive.model.c_str(), drive.sizeGb, drive.type.c_str()));
    }
    ImGui::EndTable();
}

void drawOsInfo(const StaticInfo& info, const LiveStats& live) {
    ImGui::TextColored(kInfoHeading, "Operating System Information");
    ImGui::Spacing();
    if (!beginInfoTable("OsInfoTable", 100.0f)) return;

    drawInfoRow("OS", info.os.osName);
    drawInfoRow("Kernel", info.os.kernel);
    drawInfoRow("Uptime", live.uptime);
    drawInfoRow("Packages", info.os.packages);
    drawInfoRow("Shell", info.os.shell);
    ImGui::EndTable();
}

// A square button with an icon drawn from shapes (the default font has no icon
// characters). Draws the hover highlight; the caller draws the icon at `center`
// in `color`. `active` keeps it highlighted while its menu is open.
struct IconButton {
    bool clicked;
    ImVec2 center;
    ImU32 color;
};

IconButton iconButton(const char* id, float size, bool active, const char* tooltip) {
    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("%s", tooltip);

    if (hovered || active) {
        ImGui::GetWindowDrawList()->AddRectFilled(topLeft, ImVec2(topLeft.x + size, topLeft.y + size),
                                                  ImGui::GetColorU32(ImGuiCol_FrameBgHovered), ImGui::GetStyle().FrameRounding);
    }
    return {clicked, ImVec2(topLeft.x + size * 0.5f, topLeft.y + size * 0.5f),
            ImGui::GetColorU32((hovered || active) ? ImGuiCol_Text : ImGuiCol_TextDisabled)};
}

// A gear-shaped button: a thick ring for the body plus rectangles for the teeth.
// Returns true when clicked.
bool drawGearButton(const char* id, float size, bool active) {
    constexpr int kTeeth = 8;

    const auto [clicked, center, color] = iconButton(id, size, active, "Settings");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tipRadius  = size * 0.42f;
    const float bodyRadius = tipRadius * 0.72f;
    const float holeRadius = tipRadius * 0.32f;
    const float toothHalfWidth = tipRadius * 0.18f;

    // Body: drawn as a thick outline, so the hole in the middle stays see-through.
    draw->AddCircle(center, (bodyRadius + holeRadius) * 0.5f, color, 0, bodyRadius - holeRadius);

    for (int i = 0; i < kTeeth; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / kTeeth;
        const ImVec2 out(cosf(angle), sinf(angle));                     // centre -> tooth
        const ImVec2 side(-out.y * toothHalfWidth, out.x * toothHalfWidth);
        const float baseRadius = bodyRadius - 1.0f;                     // overlap the ring: no gap
        const ImVec2 base(center.x + out.x * baseRadius, center.y + out.y * baseRadius);
        const ImVec2 tip (center.x + out.x * tipRadius,  center.y + out.y * tipRadius);
        draw->AddQuadFilled(ImVec2(base.x - side.x, base.y - side.y), ImVec2(tip.x - side.x, tip.y - side.y),
                            ImVec2(tip.x + side.x, tip.y + side.y),   ImVec2(base.x + side.x, base.y + side.y), color);
    }
    return clicked;
}

// A hard-drive-shaped button: the drive seen from above with its lid off, like
// the classic Mac hard disk icon - the case, the disk (platter) with its hub,
// and the read/write arm swinging in from a corner. Returns true when clicked.
bool drawStorageButton(const char* id, float size, bool active) {
    const auto [clicked, center, color] = iconButton(id, size, active, "View Storage Usage Stats");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float line = std::max(1.0f, std::round(size * 0.07f));

    // The case: a little taller than wide, like a 3.5" drive.
    const float halfWidth = std::round(size * 0.32f), halfHeight = std::round(size * 0.40f);
    const ImVec2 min(center.x - halfWidth, center.y - halfHeight), max(center.x + halfWidth, center.y + halfHeight);
    draw->AddRect(min, max, color, size * 0.08f, 0, line);

    // The platter fills the top of the case; the hub is a dot in its middle.
    const float platterRadius = halfWidth * 0.78f;
    const ImVec2 platter(center.x, min.y + halfWidth);
    draw->AddCircle(platter, platterRadius, color, 0, line);
    draw->AddCircleFilled(platter, std::max(1.2f, size * 0.05f), color);

    // The arm: pivots in the bottom-right corner and reaches across the platter.
    const ImVec2 pivot(max.x - halfWidth * 0.38f, max.y - halfWidth * 0.38f);
    const ImVec2 head(platter.x + platterRadius * 0.35f, platter.y + platterRadius * 0.45f);
    draw->AddLine(pivot, head, color, line);
    draw->AddCircleFilled(pivot, std::max(1.5f, size * 0.07f), color);
    return clicked;
}

// "[ 500 ms v ]" - a dropdown that changes `refreshMs` when the user picks a value.
void drawIntervalPicker(int& refreshMs) {
    ImGui::SetNextItemWidth(-FLT_MIN);   // as wide as the menu
    if (ImGui::BeginCombo("##UpdateInterval", format("%d ms", refreshMs).c_str())) {
        for (const int choice : kRefreshChoicesMs) {
            const bool isCurrent = (choice == refreshMs);
            if (ImGui::Selectable(format("%d ms", choice).c_str(), isCurrent)) refreshMs = choice;
            if (isCurrent) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// "[ JARV v ]" - a dropdown that changes `skin` when the user picks one.
void drawSkinPicker(Skin& skin) {
    ImGui::SetNextItemWidth(-FLT_MIN);   // as wide as the menu
    if (ImGui::BeginCombo("##Skin", skinChoice(skin).name)) {
        for (const SkinChoice& choice : kSkinChoices) {
            const bool isCurrent = (choice.skin == skin);
            if (ImGui::Selectable(choice.name, isCurrent)) skin = choice.skin;
            if (isCurrent) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// The small menu that drops down from the gear (gearMin/gearMax = the gear's
// rectangle). Closes when the user clicks anywhere else or presses Escape; a
// click on the gear itself is left to the gear, which toggles the menu.
void drawSettingsMenu(UiState& ui, const ImVec2& gearMin, const ImVec2& gearMax) {
    ImGui::SetNextWindowPos(ImVec2(gearMax.x, gearMax.y + 6.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(150.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextDisabled("Skin");
    drawSkinPicker(ui.settings.skin);
    ImGui::BeginDisabled(ui.settings.skin == Skin::Classic);   // Classic has nothing to animate
    ImGui::Checkbox("Animations", &ui.settings.animations);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Update Interval");
    drawIntervalPicker(ui.settings.refreshMs);

    ImGui::Separator();
    if (ImGui::RadioButton("Display Temperature in \xC2\xB0" "F", ui.settings.fahrenheit)) ui.settings.fahrenheit = !ui.settings.fahrenheit;
    if (ImGui::RadioButton("Borderless", ui.settings.borderless)) ui.settings.borderless = !ui.settings.borderless;
    ImGui::SetItemTooltip("Hide Window Borders");

    ImGui::Separator();
    ImGui::TextDisabled("Overlay (Ctrl+Shift+O)");
    ImGui::Checkbox("Limit Overlay to Focused App", &ui.settings.limitOverlayToApp);
    ImGui::SetItemTooltip("Ties Overlay to Focused App. Unchecking leaves overlay open, ignoring focused app");
    ImGui::Checkbox("Keep VeeaStats running in the background", &ui.settings.runInBackground);
    ImGui::SetItemTooltip("Will allow overlay hotkey to remain active even when VeeaStats is closed");
    if (ui.settings.runInBackground) {
        ImGui::Separator();
        if (ImGui::Selectable("Quit VeeaStats")) ui.quitRequested = true;
    }

    const bool clickedElsewhere = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                  !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                  !ImGui::IsMouseHoveringRect(gearMin, gearMax, false);   // false: the gear is outside this menu's clip area
    if (clickedElsewhere || ImGui::IsKeyPressed(ImGuiKey_Escape)) ui.settingsOpen = false;
    ImGui::End();
}

// "931.5 GB", "1.82 TB"
std::string sizeText(double gb) {
    return gb >= 1000.0 ? format("%.2f TB", gb / 1024.0) : format("%.1f GB", gb);
}

// One slice of a pie, from angle `from` to `to` (radians, clockwise from 3 o'clock).
// ImGui only fills convex shapes, so it is filled a quarter turn at a time.
void drawPieSlice(ImDrawList* draw, const ImVec2& center, float radius, float from, float to, ImU32 color) {
    for (float start = from; start < to; start += kPi * 0.5f) {
        draw->PathLineTo(center);
        draw->PathArcTo(center, radius, start, std::min(start + kPi * 0.5f, to));
        draw->PathFillConvex(color);
    }
}

// The pie chart of one device's space, with its legend beside it.
void drawDiskChart(const DiskUsage& disk) {
    struct Slice {
        const char* label;
        double gb;
        ImU32 color;
    };
    const double totalGb = disk.drive.sizeGb;
    const double otherGb = std::max(totalGb - disk.usedGb - disk.freeGb, 0.0);
    const bool mounted = !disk.mountPoints.empty();
    const Slice slices[] = {
        {"Used", disk.usedGb, ImGui::GetColorU32(ImGuiCol_CheckMark)},
        {"Free", disk.freeGb, ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.55f)},
        {mounted ? "Other" : "Not mounted", otherGb, ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.25f)},
    };

    // The pie: slices clockwise from 12 o'clock, without anti-aliasing so the
    // quarter-turn pieces join seamlessly, then an anti-aliased rim on top.
    const float radius = std::round(ImGui::GetFontSize() * 3.2f);
    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
    const ImVec2 center(topLeft.x + radius, topLeft.y + radius);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImDrawListFlags flags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    float angle = -kPi * 0.5f;
    for (const Slice& slice : slices) {
        if (totalGb <= 0.0 || slice.gb <= 0.0) continue;
        const float sweep = 2.0f * kPi * static_cast<float>(std::min(slice.gb / totalGb, 1.0));
        drawPieSlice(draw, center, radius, angle, angle + sweep, slice.color);
        angle += sweep;
    }
    draw->Flags = flags;
    draw->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), 0, 1.0f);

    // The legend, vertically centred beside the pie.
    ImGui::SameLine(0.0f, 16.0f);
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, center.y - rowHeight * 2.0f));
    ImGui::BeginGroup();
    if (ImGui::BeginTable("##Legend", 3, ImGuiTableFlags_SizingFixedFit)) {
        const float swatch = std::round(ImGui::GetFontSize() * 0.7f);
        const auto row = [&](const char* label, double gb, const ImU32* color) {
            ImGui::TableNextColumn();
            if (color) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float top = at.y + std::round((ImGui::GetTextLineHeight() - swatch) * 0.5f);
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + swatch, top + swatch), *color);
            }
            ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(totalGb > 0.0 ? format("%s  %3.0f%%", sizeText(gb).c_str(), 100.0 * gb / totalGb).c_str()
                                                 : sizeText(gb).c_str());
        };
        for (const Slice& slice : slices) {
            if (slice.gb > 0.0 || (mounted && &slice != &slices[2])) row(slice.label, slice.gb, &slice.color);
        }
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Total");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", sizeText(totalGb).c_str());
        ImGui::EndTable();
    }
    ImGui::EndGroup();

    std::string where;
    for (const std::string& mountPoint : disk.mountPoints) where += (where.empty() ? "" : ", ") + mountPoint;
    ImGui::SetCursorScreenPos(ImVec2(topLeft.x, topLeft.y + radius * 2.0f + ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextDisabled("%s", mounted ? ("Mounted at " + where).c_str() : "Not mounted: its space can't be read");
    if (mounted && otherGb > 0.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        ImGui::SetItemTooltip("Other: partitions that aren't mounted, unpartitioned space,\nand space the filesystem keeps for itself");
    }
}

// The menu that drops down from the storage button (buttonMin/buttonMax = the
// button's rectangle): one row per storage device; clicking a row shows its
// chart. Closes like the settings menu.
void drawStorageMenu(UiState& ui, const ImVec2& buttonMin, const ImVec2& buttonMax) {
    StorageMenu& menu = ui.storage;
    const double now = ImGui::GetTime();
    if (menu.reader.take(menu.disks)) menu.haveDisks = true;
    if (now - menu.readAt > 2.0) {   // fresh numbers when it opens, then every couple of seconds
        menu.reader.request();
        menu.readAt = now;
    }

    ImGui::SetNextWindowPos(ImVec2(buttonMax.x, buttonMax.y + 6.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Storage", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextDisabled("Storage Devices");
    if (!menu.haveDisks) ImGui::TextUnformatted("Reading...");
    else if (menu.disks.empty()) ImGui::TextUnformatted("No storage devices found");
    const DiskUsage* chosen = nullptr;
    if (!menu.disks.empty() && ImGui::BeginTable("##Devices", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const DiskUsage& disk : menu.disks) {
            const bool isChosen = (disk.drive.deviceName == menu.selected);
            if (isChosen) chosen = &disk;
            ImGui::TableNextColumn();
            if (ImGui::Selectable(format("%s##%s", disk.drive.model.c_str(), disk.drive.deviceName.c_str()).c_str(), isChosen,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                menu.selected = isChosen ? "" : disk.drive.deviceName;   // clicking the chosen one again hides its chart
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s  %s", sizeText(disk.drive.sizeGb).c_str(), disk.drive.type.c_str());
        }
        ImGui::EndTable();
    }
    if (chosen) {
        ImGui::Separator();
        drawDiskChart(*chosen);
    }

    const bool clickedElsewhere = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                  !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                  !ImGui::IsMouseHoveringRect(buttonMin, buttonMax, false);
    if (clickedElsewhere || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        menu.open = false;
        menu.readAt = -1.0e9;   // read again next time it opens (the last numbers show meanwhile)
    }
    ImGui::End();
}

// The whole window in the Classic skin: performance on top, system details below.
// `ui` is read and (if the user changes something) updated here.
void drawDashboard(const ImVec2& windowSize, UiState& ui, const StaticInfo& info, const LiveStats& live) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(windowSize);
    ImGui::Begin("VeeaStats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Title on the left; the storage button and settings gear pinned to the right edge of the same row.
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float buttonSize = ImGui::GetFrameHeight();
    const float buttonGap = 4.0f;
    ImGui::AlignTextToFramePadding();   // centres the title on the buttons' height
    ImGui::TextColored(kTitleColor, "System Performance Dashboard");
    ImGui::SameLine(rightEdge - buttonSize * 2.0f - buttonGap);
    if (drawStorageButton("##Storage", buttonSize, ui.storage.open)) ui.storage.open = !ui.storage.open;
    const ImVec2 storageMin = ImGui::GetItemRectMin(), storageMax = ImGui::GetItemRectMax();
    ImGui::SameLine(0.0f, buttonGap);
    if (drawGearButton("##Settings", buttonSize, ui.settingsOpen)) ui.settingsOpen = !ui.settingsOpen;
    const ImVec2 gearMin = ImGui::GetItemRectMin(), gearMax = ImGui::GetItemRectMax();

    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiTableFlags columnFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("PerformanceTable", 2, columnFlags)) {
        ImGui::TableNextColumn();   // left: CPU + RAM
        drawCpuSection(info, live, ui.settings.fahrenheit);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        drawRamSection(info, live);

        ImGui::TableNextColumn();   // right: GPU
        drawGpuSection(live, ui.settings.fahrenheit);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("SystemInfoColumnsTable", 2, columnFlags)) {
        ImGui::TableNextColumn();
        drawHardwareInfo(info, live);
        ImGui::TableNextColumn();
        drawOsInfo(info, live);
        ImGui::EndTable();
    }

    ImGui::End();

    if (ui.storage.open) drawStorageMenu(ui, storageMin, storageMax);
    if (ui.settingsOpen) drawSettingsMenu(ui, gearMin, gearMax);
}

// ============================================================================
// 6. HUD SKINS (JARV, GALACTIC CONFLICT, FEDERATION GUNSHIP, HALLOWEEN, POSEIDON, OTAKU, CYBER-PUNK, CLASSROOM)
// ============================================================================
// These skins share one layout: a header, four gauges, then two rows of panels
// (CPU cores + graphics, hardware + operating system). Each skin supplies the
// drawing for those parts through a HudSkin; the layout itself is
// drawHudDashboard() at the end of this section. A new skin of this kind only
// needs its own drawing functions and an entry in makeHudSkins() (section 7).
//
// Everything is drawn with ImGui's shape functions, so no skin needs images or
// extra libraries.

// ---- Shared by every HUD skin -----------------------------------------------

// A rectangle on screen.
struct Box {
    ImVec2 min, max;
    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
};

// Where a header put its storage and settings buttons.
struct HeaderButtons {
    Box storage, gear;
};

ImU32 withAlpha(const ImVec4& color, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

ImVec2 pointOnCircle(const ImVec2& center, float radius, float angle) {
    return ImVec2(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
}

std::string upperCase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// "VRAM: shares system RAM" -> "SHARES SYSTEM RAM".
std::string noteValue(const std::string& note) {
    const size_t colon = note.find(": ");
    return upperCase(colon == std::string::npos ? note : note.substr(colon + 2));
}

ImVec2 textSize(ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
}

// Text centred on `center`, both across and up/down.
void drawCenteredText(ImDrawList* draw, ImFont* font, float size, const ImVec2& center, ImU32 color, const std::string& text) {
    const ImVec2 extent = textSize(font, size, text);
    draw->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), color, text.c_str());
}

// The skin's fonts plus the few things it remembers between frames.
struct HudState {
    ImFont* textFont{nullptr};      // labels and values
    ImFont* displayFont{nullptr};   // headings and big numbers
    bool animate{true};
    bool fahrenheit{false};         // temperatures in °F
    float time{0.0f};               // seconds; drives the moving parts (stays 0 when not animating)
    // The numbers as currently drawn. While animating they glide towards the
    // live values instead of jumping.
    float cpu{0.0f}, ram{0.0f}, gpu{0.0f}, vram{0.0f};
    std::vector<float> cores;
    std::vector<int> coreIds;   // each core's number, for its label (see LiveStats::cpuIds)
    int coreId(int index) const { return static_cast<size_t>(index) < coreIds.size() ? coreIds[static_cast<size_t>(index)] : index; }
};

// While animating, displayed numbers move a share of the remaining distance
// each frame, so a change takes about half a second to settle.
void glide(float& shown, float target, bool animate) {
    if (!animate) {
        shown = target;
        return;
    }
    shown += (target - shown) * (1.0f - expf(-ImGui::GetIO().DeltaTime * 8.0f));
}

void glideHudValues(HudState& state, const LiveStats& live) {
    glide(state.cpu, averageCpuPercent(live), state.animate);
    glide(state.ram, live.ram.percent, state.animate);
    glide(state.gpu, live.gpu.hasLoad ? live.gpu.gpuUsage : 0.0f, state.animate);
    glide(state.vram, live.gpu.hasMemory ? live.gpu.vramUsage : 0.0f, state.animate);
    state.cores.resize(live.cpuPercent.size(), 0.0f);
    for (size_t i = 0; i < live.cpuPercent.size(); ++i) glide(state.cores[i], live.cpuPercent[i], state.animate);
    state.coreIds = live.cpuIds;
}

// What each of the four gauges shows.
struct GaugeReading {
    std::string label;          // e.g. "CPU"
    float percent;              // as drawn (smoothed)
    bool hasValue;
    float warnAt, critAt;       // 0..1: where it turns to the "busy" / "critical" colour
    std::string line1, line2;   // readouts under the gauge
};

// "4.86 GHz · 62°C": the clock, and the temperature when there is a sensor.
std::string clockAndTemperature(double clockGhz, double celsius, bool fahrenheit) {
    const std::string clock = clockGhz > 0.0 ? format("%.2f GHz", clockGhz) : "CLOCK N/A";
    return hasTemperature(celsius) ? clock + " \xC2\xB7 " + temperatureText(celsius, fahrenheit) : clock;
}

std::array<GaugeReading, 4> gaugeReadings(const HudState& state, const LiveStats& live) {
    const GpuStats& gpu = live.gpu;
    const bool gpuReports = gpu.hasLoad || gpu.hasMemory || !gpu.loadNote.empty();
    return {{
        {"CPU", state.cpu, !live.cpuPercent.empty(), 0.50f, 0.80f,
         clockAndTemperature(live.cpuFreqGhz, live.cpuTemperatureC, state.fahrenheit),
         live.cpuVoltage > 0.0 ? format("VCORE %.3f V", live.cpuVoltage) : "VCORE N/A"},
        {"MEMORY", state.ram, live.ram.totalGb > 0.0, 0.65f, 0.85f,
         format("%.1f / %.1f GB", live.ram.usedGb, live.ram.totalGb), "SYSTEM RAM"},
        {gpu.loadIsEstimate ? "GPU (EST.)" : "GPU", state.gpu, gpu.hasLoad, 0.50f, 0.85f,
         clockAndTemperature(gpu.clockGhz, gpu.temperatureC, state.fahrenheit),
         !gpuReports         ? "STATS UNAVAILABLE"
         : !gpu.hasLoad      ? noteValue(gpu.loadNote)
         : gpu.voltageV > 0.0 ? format("VDDC %.3f V", gpu.voltageV) : "VDDC N/A"},
        {"VRAM", state.vram, gpu.hasMemory, 0.65f, 0.85f,
         gpu.hasMemory ? format("%.1f / %.1f GB", gpu.vramUsedGb, gpu.vramTotalGb) : "",
         gpu.hasMemory ? "VIDEO MEMORY" : gpu.memoryNote.empty() ? "NOT REPORTED" : noteValue(gpu.memoryNote)},
    }};
}

// Round gauges side by side: four columns, each with a dial and two readout
// lines under it. Shared by the skins whose gauges are dials.
struct DialLayout {
    float columnWidth, radius, centerY;
    float centerX(const Box& box, int column) const {
        return std::floor(box.min.x + columnWidth * (static_cast<float>(column) + 0.5f));
    }
};

DialLayout dialLayout(const Box& box) {
    constexpr float kBelowDial = 16.0f;   // room for the second readout line
    DialLayout layout;
    layout.columnWidth = box.width() / 4.0f;
    layout.radius = std::floor(std::min(layout.columnWidth * 0.40f, (box.height() - kBelowDial) * 0.5f));
    layout.centerY = std::floor(box.min.y + (box.height() - kBelowDial) * 0.5f);
    return layout;
}

// How one HUD skin draws each part of the shared layout.
struct HudSkin {
    ImFont* textFont{nullptr};
    ImFont* displayFont{nullptr};
    float labelSize{10.0f};   // size of the displayFont labels in the info tables
    // Label column widths of the graphics, hardware and OS tables; 0 fits the
    // column to its longest label.
    struct { float graphics, hardware, system; } labelColumns{0.0f, 0.0f, 0.0f};

    // The window background (drawn behind everything).
    void (*background)(const ImVec2& windowSize, const HudState& state){};
    // Title, clock and the storage and settings buttons. Returns the buttons' rectangles.
    HeaderButtons (*header)(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state){};
    // The four gauges, across `box`.
    void (*gauges)(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live){};
    // A panel's frame and title. Returns the area left for its contents.
    Box (*panelFrame)(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state){};
    // The whole CPU cores panel.
    void (*cores)(ImDrawList* draw, const Box& box, const HudState& state){};
    // One "LABEL   value" row of an info table.
    void (*infoRow)(const HudState& state, const char* label, const std::string& value){};
    // ImGui colours and spacing (the settings menu, tables and scrollbars use them).
    void (*style)(ImGuiStyle& style){};
    // The info tables' label font, if not displayFont (only used to size their column).
    ImFont* labelFont{nullptr};
};

// Opens a scrolling area over a panel's content area. Fill it, then call ImGui::EndChild().
void beginHudPanel(const Box& content, const char* id) {
    ImGui::SetCursorScreenPos(content.min);
    ImGui::BeginChild(id, content.max - content.min, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
}

// An info table whose label column is `width` wide, or (if 0) fits the longest of `labels`.
bool beginHudInfoTable(const HudSkin& skin, const char* id, float width, std::initializer_list<const char*> labels) {
    if (width <= 0.0f) {
        ImFont* font = skin.labelFont ? skin.labelFont : skin.displayFont;
        for (const char* label : labels) width = std::max(width, textSize(font, skin.labelSize, label).x);
        width = std::ceil(width) + 12.0f;
    }
    return beginInfoTable(id, width);
}

void drawHudGpuPanel(const HudSkin& skin, ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const GpuStats& gpu = live.gpu;
    beginHudPanel(skin.panelFrame(draw, box, "GRAPHICS", state), "##HudGpu");
    if (beginHudInfoTable(skin, "HudGpuTable", skin.labelColumns.graphics, {"MODEL", "VENDOR", "CLOCK", "VDDC", "TEMP", "LOAD", "VRAM"})) {
        skin.infoRow(state, "MODEL", gpu.name);
        skin.infoRow(state, "VENDOR", vendorName(gpu.vendor));
        skin.infoRow(state, "CLOCK", gpu.clockGhz > 0.0 ? format("%.2f GHz", gpu.clockGhz) : "N/A");
        skin.infoRow(state, "VDDC", gpu.voltageV > 0.0 ? format("%.3f V", gpu.voltageV) : "N/A");
        skin.infoRow(state, "TEMP", temperatureText(gpu.temperatureC, state.fahrenheit));
        skin.infoRow(state, "LOAD", gpu.hasLoad ? format("%.1f%%%s", gpu.gpuUsage, gpu.loadIsEstimate ? " (estimated)" : "")
                                               : gpu.loadNote.empty() ? "N/A" : noteValue(gpu.loadNote));
        skin.infoRow(state, "VRAM", gpu.hasMemory ? format("%.2f / %.2f GB", gpu.vramUsedGb, gpu.vramTotalGb)
                                                 : gpu.memoryNote.empty() ? "N/A" : noteValue(gpu.memoryNote));
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void drawHudHardwarePanel(const HudSkin& skin, ImDrawList* draw, const Box& box, const HudState& state, const StaticInfo& info,
                          const LiveStats& live) {
    beginHudPanel(skin.panelFrame(draw, box, "HARDWARE", state), "##HudHardware");
    if (beginHudInfoTable(skin, "HudHardwareTable", skin.labelColumns.hardware, {"PROCESSOR", "MOTHERBOARD", "CHIPSET", "MEMORY", "STORAGE"})) {
        skin.infoRow(state, "PROCESSOR", info.cpuModel);
        skin.infoRow(state, "MOTHERBOARD", info.board.name);
        skin.infoRow(state, "CHIPSET", info.board.chipset);
        skin.infoRow(state, "MEMORY", format("%.2f GB", live.ram.totalGb));
        for (const std::string& module : info.ramModules) skin.infoRow(state, "", module);
        for (size_t i = 0; i < info.drives.size(); ++i) {
            const DriveInfo& drive = info.drives[i];
            skin.infoRow(state, i == 0 ? "STORAGE" : "", format("%s  %.0f GB  %s", drive.model.c_str(), drive.sizeGb, drive.type.c_str()));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void drawHudSystemPanel(const HudSkin& skin, ImDrawList* draw, const Box& box, const HudState& state, const StaticInfo& info,
                        const LiveStats& live) {
    beginHudPanel(skin.panelFrame(draw, box, "OPERATING SYSTEM", state), "##HudSystem");
    if (beginHudInfoTable(skin, "HudSystemTable", skin.labelColumns.system, {"OS", "KERNEL", "UPTIME", "PACKAGES", "SHELL"})) {
        skin.infoRow(state, "OS", info.os.osName);
        skin.infoRow(state, "KERNEL", info.os.kernel);
        skin.infoRow(state, "UPTIME", live.uptime);
        skin.infoRow(state, "PACKAGES", info.os.packages);
        skin.infoRow(state, "SHELL", info.os.shell);
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// The whole window in a HUD skin: header, four gauges, then two rows of panels.
void drawHudDashboard(const ImVec2& windowSize, UiState& ui, const HudSkin& skin, HudState& state, const StaticInfo& info,
                      const LiveStats& live) {
    state.textFont = skin.textFont;
    state.displayFont = skin.displayFont;
    state.animate = ui.settings.animations;
    state.fahrenheit = ui.settings.fahrenheit;
    state.time = state.animate ? static_cast<float>(ImGui::GetTime()) : 0.0f;
    glideHudValues(state, live);
    skin.background(windowSize, state);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(windowSize);
    ImGui::Begin("VeeaStats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();   // moves up when the window is scrolled
    const float width = std::floor(ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetContentRegionAvail().y;

    // Rows share the window's height but never get too small to read; on small
    // screens the window scrolls instead.
    constexpr float kGap = 12.0f, kHeaderHeight = 40.0f;
    const float dialsNeed = std::floor(width * 0.25f * 0.80f) + 30.0f;   // four dials side by side, plus readouts
    const float gaugeHeight = std::floor(std::min(std::clamp((height - kHeaderHeight) * 0.38f, 190.0f, 270.0f), dialsNeed));
    const float rest = std::max(height - kHeaderHeight - gaugeHeight - 3.0f * kGap, 0.0f);
    const float middleHeight = std::floor(std::max(rest * 0.5f, 150.0f));
    const float bottomHeight = std::floor(std::max(rest - middleHeight, 170.0f));
    const float split = origin.x + std::floor(width * 0.58f);   // left/right panel boundary

    const HeaderButtons buttons = skin.header(draw, Box{origin, origin + ImVec2(width, kHeaderHeight)}, ui, state);

    float y = origin.y + kHeaderHeight + kGap;
    skin.gauges(draw, Box{ImVec2(origin.x, y), ImVec2(origin.x + width, y + gaugeHeight)}, state, live);

    y += gaugeHeight + kGap;
    skin.cores(draw, Box{ImVec2(origin.x, y), ImVec2(split - kGap * 0.5f, y + middleHeight)}, state);
    drawHudGpuPanel(skin, draw, Box{ImVec2(split + kGap * 0.5f, y), ImVec2(origin.x + width, y + middleHeight)}, state, live);

    y += middleHeight + kGap;
    drawHudHardwarePanel(skin, draw, Box{ImVec2(origin.x, y), ImVec2(split - kGap * 0.5f, y + bottomHeight)}, state, info, live);
    drawHudSystemPanel(skin, draw, Box{ImVec2(split + kGap * 0.5f, y), ImVec2(origin.x + width, y + bottomHeight)}, state, info, live);
    y += bottomHeight;

    // Tell ImGui how tall everything is, so the window can scroll when it doesn't fit.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, y - origin.y));
    ImGui::End();

    if (ui.storage.open) drawStorageMenu(ui, buttons.storage.min, buttons.storage.max);
    if (ui.settingsOpen) drawSettingsMenu(ui, buttons.gear.min, buttons.gear.max);
}

// The clock and date as shown in the headers: "09:41", "05 OCT 2026".
void currentClock(std::string& clock, std::string& date) {
    char clockText[16] = "", dateText[32] = "";
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(clockText, sizeof(clockText), "%H:%M", &local);
    strftime(dateText, sizeof(dateText), "%d %b %Y", &local);
    clock = clockText;
    date = upperCase(dateText);
}

// Places the storage and settings buttons at the right end of a header row.
HeaderButtons drawHeaderButtons(UiState& ui, float right, float midY) {
    const float size = ImGui::GetFrameHeight();
    HeaderButtons buttons;
    ImGui::SetCursorScreenPos(ImVec2(right - size, midY - size * 0.5f));
    if (drawGearButton("##Settings", size, ui.settingsOpen)) ui.settingsOpen = !ui.settingsOpen;
    buttons.gear = Box{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    ImGui::SetCursorScreenPos(ImVec2(right - size * 2.0f - 4.0f, midY - size * 0.5f));
    if (drawStorageButton("##Storage", size, ui.storage.open)) ui.storage.open = !ui.storage.open;
    buttons.storage = Box{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    return buttons;
}

// ---- JARV ---------------------------------------------------------------------
// A holographic heads-up-display look: glowing cyan lines on dark navy glass,
// round gauges, segmented meters and panels with corner brackets.
//
// "Glow" is faked by drawing each shape twice more underneath it, wider and very
// faint. That costs almost nothing and reads like light bleeding off a screen.

const ImVec4 kJarvCyan    (0.25f, 0.85f, 1.00f, 1.0f);     // lines, arcs, headings
const ImVec4 kJarvIce     (0.86f, 0.97f, 1.00f, 1.0f);     // values
const ImVec4 kJarvMuted   (0.47f, 0.68f, 0.78f, 1.0f);     // labels
const ImVec4 kJarvGreen   (0.36f, 1.00f, 0.62f, 1.0f);     // the "live" light
const ImVec4 kJarvAmber   (1.00f, 0.72f, 0.28f, 1.0f);     // busy
const ImVec4 kJarvRed     (1.00f, 0.32f, 0.38f, 1.0f);     // critical
const ImVec4 kJarvGlass   (0.06f, 0.22f, 0.32f, 1.0f);     // panel fill
const ImVec4 kJarvBgTop   (0.016f, 0.035f, 0.055f, 1.0f);  // window background gradient
const ImVec4 kJarvBgBottom(0.027f, 0.075f, 0.110f, 1.0f);

// Cyan when relaxed, amber when busy, red when critical (warnAt/critAt are 0..1, as in drawUsageBar).
const ImVec4& jarvLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kJarvRed : (fraction > warnAt) ? kJarvAmber : kJarvCyan;
}

// Text with a soft halo: faint copies nudged around it, then the text itself.
void drawGlowText(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, const ImVec4& color, const std::string& text) {
    static const ImVec2 kHalo[] = {{-1.5f, 0.0f}, {1.5f, 0.0f}, {0.0f, -1.5f}, {0.0f, 1.5f}};
    for (const ImVec2& offset : kHalo) draw->AddText(font, size, pos + offset, withAlpha(color, 0.10f), text.c_str());
    draw->AddText(font, size, pos, withAlpha(color, 1.0f), text.c_str());
}

// The faint, wide passes drawn under every glowing shape: {extra thickness, opacity}.
constexpr float kGlowPasses[][2] = {{7.0f, 0.05f}, {3.5f, 0.12f}};

void drawGlowArc(ImDrawList* draw, const ImVec2& center, float radius, float fromAngle, float toAngle,
                 const ImVec4& color, float thickness, float alpha = 1.0f) {
    for (const auto& pass : kGlowPasses) {
        draw->PathArcTo(center, radius, fromAngle, toAngle);
        draw->PathStroke(withAlpha(color, pass[1] * alpha), thickness + pass[0]);
    }
    draw->PathArcTo(center, radius, fromAngle, toAngle);
    draw->PathStroke(withAlpha(color, alpha), thickness);
}

void drawGlowLine(ImDrawList* draw, const ImVec2& from, const ImVec2& to, const ImVec4& color, float thickness, float alpha = 1.0f) {
    for (const auto& pass : kGlowPasses) draw->AddLine(from, to, withAlpha(color, pass[1] * alpha), thickness + pass[0]);
    draw->AddLine(from, to, withAlpha(color, alpha), thickness);
}

// A bar made of separate blocks, like an LED meter.
void drawSegmentMeter(ImDrawList* draw, const Box& box, float percent, float warnAt, float critAt) {
    constexpr float kGap = 2.0f;
    const int count = std::clamp(static_cast<int>(box.width() / 7.0f), 6, 40);
    const float segmentWidth = (box.width() - kGap * static_cast<float>(count - 1)) / static_cast<float>(count);
    const float lit = std::clamp(percent, 0.0f, 100.0f) / 100.0f * static_cast<float>(count);
    const ImVec4& color = jarvLoadColor(percent, warnAt, critAt);

    for (int i = 0; i < count; ++i) {
        const float left = box.min.x + static_cast<float>(i) * (segmentWidth + kGap);
        const ImVec2 a(std::floor(left), box.min.y);
        const ImVec2 b(std::floor(left + segmentWidth), box.max.y);
        draw->AddRectFilled(a, b, withAlpha(kJarvCyan, 0.08f));

        const float amount = std::clamp(lit - static_cast<float>(i), 0.0f, 1.0f);   // the last block can be part-lit
        if (amount <= 0.0f) continue;
        draw->AddRectFilled(a - ImVec2(1.5f, 1.5f), b + ImVec2(1.5f, 1.5f), withAlpha(color, 0.15f * amount));
        draw->AddRectFilled(a, b, withAlpha(color, amount));
    }
}

// Dark navy gradient with a faint grid and darker edges, behind everything.
void drawJarvBackground(const ImVec2& size, const HudState&) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 top = withAlpha(kJarvBgTop, 1.0f), bottom = withAlpha(kJarvBgBottom, 1.0f);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, top, top, bottom, bottom);

    constexpr float kGridStep = 32.0f;
    const ImU32 grid = withAlpha(kJarvCyan, 0.035f);
    for (float x = kGridStep; x < size.x; x += kGridStep) draw->AddLine(ImVec2(x, 0.0f), ImVec2(x, size.y), grid);
    for (float y = kGridStep; y < size.y; y += kGridStep) draw->AddLine(ImVec2(0.0f, y), ImVec2(size.x, y), grid);

    const float edge = std::min(size.x, size.y) * 0.12f;
    const ImU32 dark = IM_COL32(0, 0, 0, 110), clear = IM_COL32(0, 0, 0, 0);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(edge, size.y), dark, clear, clear, dark);
    draw->AddRectFilledMultiColor(ImVec2(size.x - edge, 0.0f), size, clear, dark, dark, clear);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, size.y - edge), size, clear, clear, dark, dark);
}

// Bright corner brackets around `box`, `length` long.
void drawCornerBrackets(ImDrawList* draw, const Box& box, float length, ImU32 color, float thickness) {
    const ImVec2 corners[] = {box.min, ImVec2(box.max.x, box.min.y), box.max, ImVec2(box.min.x, box.max.y)};
    const ImVec2 inwards[] = {ImVec2(1.0f, 1.0f), ImVec2(-1.0f, 1.0f), ImVec2(-1.0f, -1.0f), ImVec2(1.0f, -1.0f)};
    for (int i = 0; i < 4; ++i) {
        draw->PathLineTo(ImVec2(corners[i].x + inwards[i].x * length, corners[i].y));
        draw->PathLineTo(corners[i]);
        draw->PathLineTo(ImVec2(corners[i].x, corners[i].y + inwards[i].y * length));
        draw->PathStroke(color, thickness);
    }
}

// A glass panel: faint fill, thin border, bright corner brackets and a title
// strip, plus a slow scan line while animating. Returns the area for content.
Box drawJarvPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& jarv) {
    draw->AddRectFilledMultiColor(box.min, box.max, withAlpha(kJarvGlass, 0.30f), withAlpha(kJarvGlass, 0.30f),
                                  withAlpha(kJarvGlass, 0.10f), withAlpha(kJarvGlass, 0.10f));
    draw->AddRect(box.min, box.max, withAlpha(kJarvCyan, 0.22f));
    drawCornerBrackets(draw, box, 12.0f, withAlpha(kJarvCyan, 0.9f), 2.0f);

    // Title strip: "■ TITLE ───────────── ▮▮▮"
    constexpr float kTitleSize = 11.0f;
    const ImVec2 titleExtent = textSize(jarv.displayFont, kTitleSize, title);
    const float titleY = box.min.y + 9.0f;
    const float midY = std::floor(titleY + titleExtent.y * 0.5f) + 0.5f;
    draw->AddRectFilled(ImVec2(box.min.x + 10.0f, midY - 2.5f), ImVec2(box.min.x + 15.0f, midY + 2.5f), withAlpha(kJarvCyan, 1.0f));
    drawGlowText(draw, jarv.displayFont, kTitleSize, ImVec2(box.min.x + 22.0f, titleY), kJarvCyan, title);
    const float lineLeft = box.min.x + 32.0f + titleExtent.x, lineRight = box.max.x - 34.0f;
    if (lineRight > lineLeft) draw->AddLine(ImVec2(lineLeft, midY), ImVec2(lineRight, midY), withAlpha(kJarvCyan, 0.22f));
    for (int i = 0; i < 3; ++i) {
        const float x = box.max.x - 28.0f + 6.0f * static_cast<float>(i);
        draw->AddRectFilled(ImVec2(x, midY - 3.0f), ImVec2(x + 3.0f, midY + 3.0f), withAlpha(kJarvCyan, 0.3f + 0.25f * static_cast<float>(i)));
    }

    // Scan line: a faint band that sweeps down the panel every few seconds.
    // Each panel starts at a different point so they don't move in lockstep.
    if (jarv.animate) {
        constexpr float kSweepSeconds = 6.0f;
        const float phase = fmodf(jarv.time + (box.min.x + box.min.y) * 0.01f, kSweepSeconds) / kSweepSeconds;
        const float y = box.min.y + phase * (box.height() + 60.0f) - 30.0f;
        const ImU32 clear = withAlpha(kJarvCyan, 0.0f), faint = withAlpha(kJarvCyan, 0.06f);
        draw->PushClipRect(box.min, box.max, true);
        draw->AddRectFilledMultiColor(ImVec2(box.min.x, y - 30.0f), ImVec2(box.max.x, y), clear, clear, faint, faint);
        draw->AddLine(ImVec2(box.min.x, y), ImVec2(box.max.x, y), withAlpha(kJarvCyan, 0.16f));
        draw->PopClipRect();
    }
    return Box{ImVec2(box.min.x + 12.0f, titleY + titleExtent.y + 10.0f), ImVec2(box.max.x - 8.0f, box.max.y - 8.0f)};
}

// A "LABEL   value" table row: small Orbitron label, larger Rajdhani value.
void drawJarvInfoRow(const HudState& jarv, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(jarv.displayFont, 10.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);   // line the small label up with the taller value
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// A round "arc reactor" gauge: a ring of tick marks, a segmented 270-degree
// value arc, slowly turning inner rings and the number in the middle.
void drawJarvGauge(ImDrawList* draw, const ImVec2& center, float radius, const GaugeReading& reading, const HudState& jarv) {
    constexpr float kStart = kPi * 0.75f;   // the arc runs clockwise from bottom-left...
    constexpr float kSweep = kPi * 1.5f;    // ...round to bottom-right, leaving the bottom open

    // Tick marks every 5 degrees (longer every 45), except in the bottom opening.
    for (int i = 0; i < 72; ++i) {
        if (i > 9 && i < 27) continue;
        const float angle = 2.0f * kPi * static_cast<float>(i) / 72.0f;
        const bool major = (i % 9 == 0);
        draw->AddLine(pointOnCircle(center, radius * (major ? 0.91f : 0.95f), angle), pointOnCircle(center, radius, angle),
                      withAlpha(kJarvCyan, major ? 0.6f : 0.2f), 1.0f);
    }

    // The value arc, in 40 separate segments.
    constexpr int kSegments = 40;
    const float step = kSweep / kSegments;
    const float arcRadius = radius * 0.79f;
    const float thickness = std::max(radius * 0.10f, 4.0f);
    const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
    const float lit = reading.hasValue ? percent / 100.0f * kSegments : 0.0f;
    const ImVec4& color = jarvLoadColor(percent, reading.warnAt, reading.critAt);
    for (int i = 0; i < kSegments; ++i) {
        const float from = kStart + step * static_cast<float>(i);
        const float to = from + step * 0.75f;
        draw->PathArcTo(center, arcRadius, from, to);
        draw->PathStroke(withAlpha(kJarvCyan, 0.10f), thickness);
        const float amount = std::clamp(lit - static_cast<float>(i), 0.0f, 1.0f);
        if (amount > 0.0f) drawGlowArc(draw, center, arcRadius, from, to, color, thickness, amount);
    }

    // Thin guide lines either side of the arc, and a bright marker at the current value.
    for (const float edge : {arcRadius + thickness, arcRadius - thickness}) {
        draw->PathArcTo(center, edge, kStart, kStart + kSweep);
        draw->PathStroke(withAlpha(kJarvCyan, 0.22f), 1.0f);
    }
    if (reading.hasValue) {
        const ImVec2 marker = pointOnCircle(center, arcRadius + thickness, kStart + kSweep * percent / 100.0f);
        draw->AddCircleFilled(marker, 5.0f, withAlpha(kJarvIce, 0.18f));
        draw->AddCircleFilled(marker, 2.5f, withAlpha(kJarvIce, 1.0f));
    }

    // Inner rings: three long dashes turning one way, a ring of short dashes the other.
    const float spin = jarv.time * 0.35f;
    for (int i = 0; i < 3; ++i) {
        const float from = spin + 2.0f * kPi * static_cast<float>(i) / 3.0f;
        drawGlowArc(draw, center, radius * 0.60f, from, from + 1.2f, kJarvCyan, 1.5f, 0.6f);
    }
    for (int i = 0; i < 24; ++i) {
        const float from = -spin * 0.6f + 2.0f * kPi * static_cast<float>(i) / 24.0f;
        draw->PathArcTo(center, radius * 0.545f, from, from + 0.13f);
        draw->PathStroke(withAlpha(kJarvCyan, 0.30f), 1.5f);
    }
    draw->AddCircleFilled(center, radius * 0.49f, withAlpha(kJarvBgTop, 0.7f));
    draw->AddCircle(center, radius * 0.49f, withAlpha(kJarvCyan, 0.25f), 0, 1.0f);

    // The number in the middle (with a small "%"), and the label under it.
    ImFont* display = jarv.displayFont;
    const float numberSize = std::round(radius * 0.36f);
    if (reading.hasValue) {
        const std::string number = format("%.0f", percent);
        const float signSize = std::round(numberSize * 0.45f);
        const ImVec2 numberExtent = textSize(display, numberSize, number);
        const ImVec2 signExtent = textSize(display, signSize, "%");
        const ImVec2 pos(center.x - (numberExtent.x + signExtent.x + 2.0f) * 0.5f, center.y - numberExtent.y * 0.65f);
        drawGlowText(draw, display, numberSize, pos, kJarvIce, number);
        draw->AddText(display, signSize, ImVec2(pos.x + numberExtent.x + 2.0f, pos.y + numberExtent.y - signExtent.y - numberSize * 0.12f),
                      withAlpha(kJarvCyan, 1.0f), "%");
    } else {
        drawCenteredText(draw, display, std::round(numberSize * 0.7f), ImVec2(center.x, center.y - numberSize * 0.25f),
                         withAlpha(kJarvMuted, 1.0f), "N/A");
    }
    drawCenteredText(draw, display, std::max(std::round(radius * 0.12f), 9.0f), ImVec2(center.x, center.y + radius * 0.24f),
                     withAlpha(kJarvMuted, 1.0f), reading.label);

    // Readouts: the first sits in the arc's opening, the second just below the dial.
    drawCenteredText(draw, jarv.textFont, 17.0f, ImVec2(center.x, center.y + radius * 0.80f), withAlpha(kJarvIce, 1.0f), reading.line1);
    drawCenteredText(draw, jarv.textFont, 15.0f, ImVec2(center.x, center.y + radius * 0.80f + 19.0f), withAlpha(kJarvMuted, 1.0f), reading.line2);
}

void drawJarvGauges(ImDrawList* draw, const Box& box, const HudState& jarv, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(jarv, live);
    const DialLayout layout = dialLayout(box);
    for (int i = 0; i < 4; ++i) {
        drawJarvGauge(draw, ImVec2(layout.centerX(box, i), layout.centerY), layout.radius, readings[i], jarv);
        if (i > 0) {
            const float x = box.min.x + layout.columnWidth * static_cast<float>(i);
            draw->AddLine(ImVec2(x, layout.centerY - layout.radius * 0.6f), ImVec2(x, layout.centerY + layout.radius * 0.6f),
                          withAlpha(kJarvCyan, 0.12f));
        }
    }
}

void drawJarvCores(ImDrawList* draw, const Box& box, const HudState& jarv) {
    beginHudPanel(drawJarvPanelFrame(draw, box, format("CORE MATRIX // %zu THREADS", jarv.cores.size()), jarv), "##JarvCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    // As many columns as needed to fit every core without scrolling (up to 4).
    constexpr float kRowHeight = 22.0f;
    const int count = static_cast<int>(jarv.cores.size());
    const int rowsThatFit = std::max(1, static_cast<int>(room.y / kRowHeight));
    const int columns = std::clamp((count + rowsThatFit - 1) / rowsThatFit, 1, 4);
    const int rows = (count + columns - 1) / columns;
    const float columnWidth = std::floor(room.x / static_cast<float>(columns));

    for (int i = 0; i < count; ++i) {
        const float left = start.x + columnWidth * static_cast<float>(i / rows);   // cores run down each column
        const float midY = std::floor(start.y + kRowHeight * (static_cast<float>(i % rows) + 0.5f));
        const std::string name = format("C%02d", jarv.coreId(i));
        const ImVec2 nameExtent = textSize(jarv.displayFont, 10.0f, name);
        panel->AddText(jarv.displayFont, 10.0f, ImVec2(left, midY - nameExtent.y * 0.5f), withAlpha(kJarvMuted, 1.0f), name.c_str());

        drawSegmentMeter(panel, Box{ImVec2(left + 34.0f, midY - 4.0f), ImVec2(left + columnWidth - 56.0f, midY + 4.0f)},
                         jarv.cores[static_cast<size_t>(i)], 0.50f, 0.80f);

        const std::string value = format("%.0f%%", jarv.cores[static_cast<size_t>(i)]);
        const ImVec2 valueExtent = textSize(jarv.textFont, 17.0f, value);
        panel->AddText(jarv.textFont, 17.0f, ImVec2(left + columnWidth - 14.0f - valueExtent.x, midY - valueExtent.y * 0.5f),
                       withAlpha(kJarvIce, 1.0f), value.c_str());
    }
    ImGui::Dummy(ImVec2(room.x, kRowHeight * static_cast<float>(rows)));
    ImGui::EndChild();
}

// Title on the left; live light, clock and the gear on the
// right; a glowing rule underneath. Returns the buttons' rectangles.
HeaderButtons drawJarvHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& jarv) {
    ImFont* display = jarv.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 2.0f;

    const ImVec2 titleExtent = textSize(display, 20.0f, "VEEASTATS");
    drawGlowText(draw, display, 20.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), kJarvCyan, "VEEASTATS");
    const ImVec2 subtitleExtent = textSize(display, 10.0f, "// SYSTEM DIAGNOSTICS");
    draw->AddText(display, 10.0f, ImVec2(box.min.x + titleExtent.x + 12.0f, midY - subtitleExtent.y * 0.5f + 2.0f),
                  withAlpha(kJarvMuted, 1.0f), "// SYSTEM DIAGNOSTICS");

    // Right-hand side, placed from the right edge inwards.
    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 20.0f;

    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(display, 15.0f, clock);
    right -= clockExtent.x;
    draw->AddText(display, 15.0f, ImVec2(right, midY - clockExtent.y * 0.5f), withAlpha(kJarvIce, 1.0f), clock.c_str());
    const ImVec2 dateExtent = textSize(display, 10.0f, date);
    right -= dateExtent.x + 10.0f;
    draw->AddText(display, 10.0f, ImVec2(right, midY - dateExtent.y * 0.5f + 1.0f), withAlpha(kJarvMuted, 1.0f), date.c_str());

    // "● LIVE" - the light pulses gently while animating.
    const float pulse = jarv.animate ? 0.6f + 0.4f * sinf(jarv.time * 3.0f) : 1.0f;
    const ImVec2 liveExtent = textSize(display, 10.0f, "LIVE");
    right -= liveExtent.x + 18.0f;
    draw->AddCircleFilled(ImVec2(right - 8.0f, midY), 7.0f, withAlpha(kJarvGreen, 0.15f * pulse));
    draw->AddCircleFilled(ImVec2(right - 8.0f, midY), 3.5f, withAlpha(kJarvGreen, pulse));
    draw->AddText(display, 10.0f, ImVec2(right + 2.0f, midY - liveExtent.y * 0.5f + 1.0f), withAlpha(kJarvGreen, 1.0f), "LIVE");

    // The rule: faint across the full width, bright where it starts.
    const float ruleY = box.max.y - 0.5f;
    draw->AddLine(ImVec2(box.min.x, ruleY), ImVec2(box.max.x, ruleY), withAlpha(kJarvCyan, 0.25f));
    drawGlowLine(draw, ImVec2(box.min.x, ruleY), ImVec2(box.min.x + 160.0f, ruleY), kJarvCyan, 2.0f);
    for (int i = 0; i < 4; ++i) {
        const float x = box.max.x - 6.0f * static_cast<float>(i) - 3.0f;
        draw->AddRectFilled(ImVec2(x - 3.0f, ruleY - 3.0f), ImVec2(x, ruleY + 1.0f), withAlpha(kJarvCyan, 0.6f));
    }
    return buttons;
}

void applyJarvStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 8.0f;

    auto cyan = [](float alpha) { return ImVec4(kJarvCyan.x, kJarvCyan.y, kJarvCyan.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kJarvIce;
    colors[ImGuiCol_TextDisabled]         = kJarvMuted;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.02f, 0.05f, 0.08f, 0.96f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.02f, 0.05f, 0.08f, 0.96f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = cyan(0.45f);
    colors[ImGuiCol_FrameBg]              = cyan(0.08f);
    colors[ImGuiCol_FrameBgHovered]       = cyan(0.22f);
    colors[ImGuiCol_FrameBgActive]        = cyan(0.32f);
    colors[ImGuiCol_Button]               = cyan(0.12f);
    colors[ImGuiCol_ButtonHovered]        = cyan(0.28f);
    colors[ImGuiCol_ButtonActive]         = cyan(0.40f);
    colors[ImGuiCol_Header]               = cyan(0.20f);
    colors[ImGuiCol_HeaderHovered]        = cyan(0.30f);
    colors[ImGuiCol_HeaderActive]         = cyan(0.40f);
    colors[ImGuiCol_CheckMark]            = kJarvCyan;
    colors[ImGuiCol_Separator]            = cyan(0.25f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = cyan(0.25f);
    colors[ImGuiCol_ScrollbarGrabHovered] = cyan(0.40f);
    colors[ImGuiCol_ScrollbarGrabActive]  = cyan(0.55f);
    colors[ImGuiCol_TextSelectedBg]       = cyan(0.25f);
    colors[ImGuiCol_NavCursor]            = kJarvCyan;
}

// ---- Galactic Conflict --------------------------------------------------------
// A starship tactical display: thin electric-blue line work on black, rings of
// fine tick marks, bar graphs on a grid, and notched panels with gold titles,
// like a capital ship's targeting screens and a space-war game's main menu.

const ImVec4 kGcBlue  (0.30f, 0.40f, 1.00f, 1.0f);      // line work
const ImVec4 kGcSky   (0.58f, 0.67f, 1.00f, 1.0f);      // labels
const ImVec4 kGcWhite (0.92f, 0.94f, 1.00f, 1.0f);      // values
const ImVec4 kGcGold  (1.00f, 0.80f, 0.20f, 1.0f);      // titles; busy
const ImVec4 kGcRed   (1.00f, 0.20f, 0.25f, 1.0f);      // markers; critical
const ImVec4 kGcPanel (0.03f, 0.05f, 0.17f, 1.0f);      // panel fill
const ImVec4 kGcBlack (0.006f, 0.008f, 0.022f, 1.0f);   // window background

// Blue when relaxed, gold when busy, red when critical.
const ImVec4& galacticLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kGcRed : (fraction > warnAt) ? kGcGold : kGcBlue;
}

// A repeatable "random" number in 0..1 for decoration that must not flicker.
float hash01(int n) {
    unsigned x = static_cast<unsigned>(n) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return static_cast<float>(x & 0xffffu) / 65535.0f;
}

// Black, a fine blue grid, faint perspective lines running in from the corners
// (like a hologram's frame), and columns of short dashes down both edges that
// read like streaming data in an alien script.
void drawGalacticBackground(const ImVec2& size, const HudState& state) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, withAlpha(kGcBlack, 1.0f));

    constexpr float kGridStep = 40.0f;
    const ImU32 grid = withAlpha(kGcBlue, 0.06f);
    for (float x = kGridStep; x < size.x; x += kGridStep) draw->AddLine(ImVec2(x, 0.0f), ImVec2(x, size.y), grid);
    for (float y = kGridStep; y < size.y; y += kGridStep) draw->AddLine(ImVec2(0.0f, y), ImVec2(size.x, y), grid);

    const ImVec2 center = size * 0.5f;
    for (const ImVec2& corner : {ImVec2(0, 0), ImVec2(size.x, 0), size, ImVec2(0, size.y)}) {
        draw->AddLine(corner, corner + (center - corner) * 0.45f, withAlpha(kGcBlue, 0.10f));
    }

    // The dashes drift slowly upwards while animating.
    constexpr float kRowStep = 7.0f;
    const int scroll = static_cast<int>(state.time * 4.0f);
    for (float y = 50.0f; y < size.y - 10.0f; y += kRowStep) {
        const int row = static_cast<int>(y / kRowStep) + scroll;
        const float leftLength = 2.0f + std::floor(hash01(row) * 7.0f);
        const float rightLength = 2.0f + std::floor(hash01(row + 9973) * 7.0f);
        draw->AddRectFilled(ImVec2(3.0f, y), ImVec2(3.0f + leftLength, y + 2.0f), withAlpha(kGcSky, 0.22f));
        draw->AddRectFilled(ImVec2(size.x - 3.0f - rightLength, y), ImVec2(size.x - 3.0f, y + 2.0f), withAlpha(kGcSky, 0.22f));
    }
}

// The six corners of a notched rectangle: top-left and bottom-right cut off by `notch`.
std::array<ImVec2, 6> notchedCorners(const Box& box, float notch) {
    return {{ImVec2(box.min.x + notch, box.min.y), ImVec2(box.max.x, box.min.y), ImVec2(box.max.x, box.max.y - notch),
             ImVec2(box.max.x - notch, box.max.y), ImVec2(box.min.x, box.max.y), ImVec2(box.min.x, box.min.y + notch)}};
}

// A notched panel with a header band carrying the title in gold, like the
// buttons of a game menu. Returns the area for content.
Box drawGalacticPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    constexpr float kNotch = 12.0f, kHeader = 24.0f;
    const std::array<ImVec2, 6> outline = notchedCorners(box, kNotch);
    draw->AddConvexPolyFilled(outline.data(), static_cast<int>(outline.size()), withAlpha(kGcPanel, 0.72f));

    const ImVec2 band[] = {outline[0], outline[1], ImVec2(box.max.x, box.min.y + kHeader), ImVec2(box.min.x, box.min.y + kHeader),
                           outline[5]};
    draw->AddConvexPolyFilled(band, 5, withAlpha(kGcBlue, 0.16f));
    draw->AddLine(ImVec2(box.min.x, box.min.y + kHeader), ImVec2(box.max.x, box.min.y + kHeader), withAlpha(kGcBlue, 0.45f));
    draw->AddPolyline(outline.data(), static_cast<int>(outline.size()), withAlpha(kGcBlue, 0.70f), ImDrawFlags_Closed, 1.0f);

    // End caps on both sides of the header, a red marker and the gold title.
    for (const float x : {box.min.x - 2.0f, box.max.x - 1.0f}) {
        draw->AddRectFilled(ImVec2(x, box.min.y + kHeader - 7.0f), ImVec2(x + 3.0f, box.min.y + kHeader + 7.0f), withAlpha(kGcSky, 0.9f));
    }
    constexpr float kTitleSize = 10.0f;
    const ImVec2 titleExtent = textSize(state.displayFont, kTitleSize, title);
    const float midY = std::floor(box.min.y + (kHeader - titleExtent.y) * 0.5f);
    draw->AddRectFilled(ImVec2(box.min.x + kNotch + 2.0f, midY + titleExtent.y * 0.5f - 2.0f),
                        ImVec2(box.min.x + kNotch + 6.0f, midY + titleExtent.y * 0.5f + 2.0f), withAlpha(kGcRed, 1.0f));
    draw->AddText(state.displayFont, kTitleSize, ImVec2(box.min.x + kNotch + 12.0f, midY), withAlpha(kGcGold, 1.0f), title.c_str());

    // Three small tick groups at the right of the header.
    for (int group = 0; group < 3; ++group) {
        for (int tick = 0; tick < 3; ++tick) {
            const float x = box.max.x - 52.0f + static_cast<float>(group) * 14.0f + static_cast<float>(tick) * 3.0f;
            draw->AddLine(ImVec2(x, box.min.y + 8.0f), ImVec2(x, box.min.y + kHeader - 8.0f), withAlpha(kGcSky, 0.45f));
        }
    }
    return Box{ImVec2(box.min.x + 14.0f, box.min.y + kHeader + 8.0f), ImVec2(box.max.x - 10.0f, box.max.y - 10.0f)};
}

// "LABEL   value": a small wide label in pale blue, the value in white.
void drawGalacticInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.displayFont, 9.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);   // line the small label up with the taller value
    ImGui::TextColored(kGcSky, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// A targeting dial: an outer ring of fine ticks that light up to the value, a
// segmented arc inside it, a slowly turning dashed ring, crosshairs, and the
// number. Both rings run 300 degrees clockwise from bottom-left, leaving the
// bottom open for the readout.
void drawGalacticGauge(ImDrawList* draw, const ImVec2& center, float radius, const GaugeReading& reading, const HudState& state) {
    const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
    const float fraction = reading.hasValue ? percent / 100.0f : 0.0f;
    const ImVec4& color = galacticLoadColor(percent, reading.warnAt, reading.critAt);
    constexpr float kStart = kPi * (0.5f + 1.0f / 6.0f), kSweep = kPi * 5.0f / 3.0f;

    // Outer comb: 101 fine ticks (one per percent), lit up to the value.
    constexpr int kTicks = 100;
    for (int i = 0; i <= kTicks; ++i) {
        const float angle = kStart + kSweep * static_cast<float>(i) / kTicks;
        const bool lit = reading.hasValue && static_cast<float>(i) <= fraction * kTicks;
        const float inner = radius * ((i % 10 == 0) ? 0.85f : 0.90f);
        draw->AddLine(pointOnCircle(center, inner, angle), pointOnCircle(center, radius, angle),
                      lit ? withAlpha(color, 0.95f) : withAlpha(kGcBlue, 0.22f), lit ? 1.5f : 1.0f);
    }
    // A red pointer just outside the ring, at the value.
    if (reading.hasValue) {
        const float angle = kStart + kSweep * fraction;
        const ImVec2 tip = pointOnCircle(center, radius + 2.0f, angle);
        draw->AddTriangleFilled(tip, pointOnCircle(center, radius + 9.0f, angle - 0.06f), pointOnCircle(center, radius + 9.0f, angle + 0.06f),
                                withAlpha(kGcRed, 1.0f));
    }

    // Segmented arc.
    constexpr int kSegments = 30;
    const float arcRadius = radius * 0.73f;
    const float thickness = std::max(radius * 0.09f, 3.0f);
    const float lit = fraction * kSegments;
    for (int i = 0; i < kSegments; ++i) {
        const float from = kStart + kSweep * static_cast<float>(i) / kSegments;
        const float to = from + kSweep / kSegments * 0.7f;
        const float amount = std::clamp(lit - static_cast<float>(i), 0.0f, 1.0f);
        draw->PathArcTo(center, arcRadius, from, to);
        draw->PathStroke(amount > 0.0f ? withAlpha(color, 0.25f + 0.75f * amount) : withAlpha(kGcBlue, 0.12f), thickness);
    }

    // Turning dashed ring, a thin circle and crosshair ticks.
    const float spin = state.time * 0.25f;
    for (int i = 0; i < 36; ++i) {
        const float from = spin + 2.0f * kPi * static_cast<float>(i) / 36.0f;
        draw->PathArcTo(center, radius * 0.61f, from, from + 0.09f);
        draw->PathStroke(withAlpha(kGcSky, 0.35f), 1.0f);
    }
    draw->AddCircle(center, radius * 0.54f, withAlpha(kGcBlue, 0.35f), 0, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const float angle = kPi * 0.5f * static_cast<float>(i);
        draw->AddLine(pointOnCircle(center, radius * 0.50f, angle), pointOnCircle(center, radius * 0.58f, angle), withAlpha(kGcSky, 0.6f));
    }

    // The number in the middle (with a small "%": Michroma's own looks like "o/o"),
    // and the gold label under it.
    const float numberSize = std::round(radius * 0.28f);
    if (reading.hasValue) {
        const std::string number = format("%.0f", percent);
        const float signSize = std::round(numberSize * 0.75f);
        const ImVec2 numberExtent = textSize(state.displayFont, numberSize, number);
        const ImVec2 signExtent = textSize(state.textFont, signSize, "%");
        const ImVec2 pos(std::floor(center.x - (numberExtent.x + signExtent.x + 3.0f) * 0.5f), std::floor(center.y - numberExtent.y * 0.62f));
        draw->AddText(state.displayFont, numberSize, pos, withAlpha(kGcWhite, 1.0f), number.c_str());
        draw->AddText(state.textFont, signSize, ImVec2(pos.x + numberExtent.x + 3.0f, pos.y + numberExtent.y - signExtent.y),
                      withAlpha(kGcSky, 1.0f), "%");
    } else {
        drawCenteredText(draw, state.displayFont, std::round(numberSize * 0.7f), ImVec2(center.x, center.y - numberSize * 0.2f),
                         withAlpha(kGcSky, 1.0f), "N/A");
    }
    drawCenteredText(draw, state.displayFont, std::max(std::round(radius * 0.10f), 8.0f), ImVec2(center.x, center.y + radius * 0.25f),
                     withAlpha(kGcGold, 1.0f), reading.label);

    // Readouts: the first sits in the arc's opening, the second just below the dial.
    drawCenteredText(draw, state.textFont, 16.0f, ImVec2(center.x, center.y + radius * 0.82f), withAlpha(kGcWhite, 1.0f), reading.line1);
    drawCenteredText(draw, state.textFont, 14.0f, ImVec2(center.x, center.y + radius * 0.82f + 18.0f), withAlpha(kGcSky, 1.0f), reading.line2);
}

void drawGalacticGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    const DialLayout layout = dialLayout(box);
    for (int i = 0; i < 4; ++i) {
        drawGalacticGauge(draw, ImVec2(layout.centerX(box, i), layout.centerY), layout.radius * 0.94f, readings[i], state);
        if (i > 0) {   // divider: a thin line with a small diamond in the middle
            const float x = std::floor(box.min.x + layout.columnWidth * static_cast<float>(i)) + 0.5f;
            draw->AddLine(ImVec2(x, layout.centerY - layout.radius * 0.7f), ImVec2(x, layout.centerY + layout.radius * 0.7f),
                          withAlpha(kGcBlue, 0.25f));
            draw->AddQuadFilled(ImVec2(x, layout.centerY - 4.0f), ImVec2(x + 4.0f, layout.centerY), ImVec2(x, layout.centerY + 4.0f),
                                ImVec2(x - 4.0f, layout.centerY), withAlpha(kGcSky, 0.7f));
        }
    }
}

// The cores as a bar graph on a grid, with a dashed red line at the busy mark.
void drawGalacticCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawGalacticPanelFrame(draw, box, format("CORE ARRAY // %zu THREADS", state.cores.size()), state), "##GalacticCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kValueRow = 14.0f, kLabelRow = 14.0f;
    const Box graph{ImVec2(start.x, start.y + kValueRow), ImVec2(start.x + room.x, start.y + std::max(room.y - kLabelRow, kValueRow + 30.0f))};
    for (int i = 0; i <= 4; ++i) {   // 0, 25, 50, 75 and 100%
        const float y = std::floor(graph.max.y - graph.height() * static_cast<float>(i) / 4.0f) + 0.5f;
        panel->AddLine(ImVec2(graph.min.x, y), ImVec2(graph.max.x, y), withAlpha(kGcBlue, i == 0 ? 0.55f : 0.14f));
    }

    const int count = static_cast<int>(state.cores.size());
    const float slot = graph.width() / static_cast<float>(std::max(count, 1));
    const float barWidth = std::max(std::floor(slot * 0.6f), 3.0f);
    for (int i = 0; i < count; ++i) {
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float centerX = std::floor(graph.min.x + slot * (static_cast<float>(i) + 0.5f));
        const float top = std::floor(graph.max.y - graph.height() * value / 100.0f);
        const ImVec4& color = galacticLoadColor(value, 0.50f, 0.80f);
        const ImVec2 a(centerX - barWidth * 0.5f, top), b(centerX + barWidth * 0.5f, graph.max.y);
        panel->AddRectFilled(ImVec2(a.x, graph.min.y), b, withAlpha(kGcBlue, 0.06f));
        panel->AddRectFilledMultiColor(a, b, withAlpha(color, 0.85f), withAlpha(color, 0.85f), withAlpha(color, 0.25f), withAlpha(color, 0.25f));
        panel->AddRectFilled(a, ImVec2(b.x, a.y + 2.0f), withAlpha(kGcWhite, 0.9f));

        drawCenteredText(panel, state.displayFont, 8.0f, ImVec2(centerX, graph.max.y + kLabelRow * 0.5f + 1.0f), withAlpha(kGcSky, 0.9f),
                         format("%02d", i));
        if (slot >= 24.0f) {
            drawCenteredText(panel, state.textFont, 13.0f, ImVec2(centerX, std::max(top - 8.0f, start.y + 6.0f)), withAlpha(kGcWhite, 0.9f),
                             format("%.0f", value));
        }
    }

    // The busy mark (80%) as a dashed red line.
    const float busyY = std::floor(graph.max.y - graph.height() * 0.80f) + 0.5f;
    for (float x = graph.min.x; x < graph.max.x; x += 10.0f) {
        panel->AddLine(ImVec2(x, busyY), ImVec2(std::min(x + 6.0f, graph.max.x), busyY), withAlpha(kGcRed, 0.75f));
    }
    ImGui::Dummy(room);
    ImGui::EndChild();
}

// White title with a gold subtitle; the clock, a blinking red "LIVE" light and
// the gear on the right; a double rule underneath. Returns the buttons' rectangles.
HeaderButtons drawGalacticHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 3.0f;

    const ImVec2 titleExtent = textSize(display, 18.0f, "VEEASTATS");
    draw->AddText(display, 18.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), withAlpha(kGcWhite, 1.0f), "VEEASTATS");
    const float subtitleX = box.min.x + titleExtent.x + 14.0f;
    draw->AddRectFilled(ImVec2(subtitleX, midY - 2.0f), ImVec2(subtitleX + 4.0f, midY + 2.0f), withAlpha(kGcRed, 1.0f));
    const ImVec2 subtitleExtent = textSize(display, 9.0f, "TACTICAL READOUT");
    draw->AddText(display, 9.0f, ImVec2(subtitleX + 10.0f, midY - subtitleExtent.y * 0.5f), withAlpha(kGcGold, 1.0f), "TACTICAL READOUT");

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 20.0f;

    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(display, 14.0f, clock);
    right -= clockExtent.x;
    draw->AddText(display, 14.0f, ImVec2(right, midY - clockExtent.y * 0.5f), withAlpha(kGcWhite, 1.0f), clock.c_str());
    const ImVec2 dateExtent = textSize(display, 9.0f, date);
    right -= dateExtent.x + 12.0f;
    draw->AddText(display, 9.0f, ImVec2(right, midY - dateExtent.y * 0.5f), withAlpha(kGcSky, 1.0f), date.c_str());

    // "■ LIVE": the square blinks while animating.
    const bool lightOn = !state.animate || fmodf(state.time, 1.2f) < 0.8f;
    const ImVec2 liveExtent = textSize(display, 9.0f, "LIVE");
    right -= liveExtent.x + 20.0f;
    if (lightOn) draw->AddRectFilled(ImVec2(right - 9.0f, midY - 3.0f), ImVec2(right - 3.0f, midY + 3.0f), withAlpha(kGcRed, 1.0f));
    draw->AddRect(ImVec2(right - 10.0f, midY - 4.0f), ImVec2(right - 2.0f, midY + 4.0f), withAlpha(kGcRed, 0.6f));
    draw->AddText(display, 9.0f, ImVec2(right + 3.0f, midY - liveExtent.y * 0.5f), withAlpha(kGcRed, 1.0f), "LIVE");

    // Double rule, with a gold stretch at the start and a red block at the end.
    const float ruleY = std::floor(box.max.y) - 4.5f;
    draw->AddLine(ImVec2(box.min.x, ruleY), ImVec2(box.max.x, ruleY), withAlpha(kGcBlue, 0.6f));
    draw->AddLine(ImVec2(box.min.x, ruleY + 3.0f), ImVec2(box.max.x, ruleY + 3.0f), withAlpha(kGcBlue, 0.25f));
    draw->AddRectFilled(ImVec2(box.min.x, ruleY - 1.0f), ImVec2(box.min.x + 140.0f, ruleY + 1.0f), withAlpha(kGcGold, 0.9f));
    draw->AddRectFilled(ImVec2(box.max.x - 18.0f, ruleY - 2.0f), ImVec2(box.max.x, ruleY + 2.0f), withAlpha(kGcRed, 0.9f));
    return buttons;
}

void applyGalacticStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 8.0f;

    auto blue = [](float alpha) { return ImVec4(kGcBlue.x, kGcBlue.y, kGcBlue.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kGcWhite;
    colors[ImGuiCol_TextDisabled]         = kGcSky;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.01f, 0.015f, 0.05f, 0.97f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.01f, 0.015f, 0.05f, 0.97f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = blue(0.60f);
    colors[ImGuiCol_FrameBg]              = blue(0.10f);
    colors[ImGuiCol_FrameBgHovered]       = blue(0.24f);
    colors[ImGuiCol_FrameBgActive]        = blue(0.34f);
    colors[ImGuiCol_Button]               = blue(0.14f);
    colors[ImGuiCol_ButtonHovered]        = blue(0.30f);
    colors[ImGuiCol_ButtonActive]         = blue(0.42f);
    colors[ImGuiCol_Header]               = blue(0.24f);
    colors[ImGuiCol_HeaderHovered]        = blue(0.34f);
    colors[ImGuiCol_HeaderActive]         = blue(0.44f);
    colors[ImGuiCol_CheckMark]            = kGcGold;
    colors[ImGuiCol_Separator]            = blue(0.35f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = blue(0.35f);
    colors[ImGuiCol_ScrollbarGrabHovered] = blue(0.50f);
    colors[ImGuiCol_ScrollbarGrabActive]  = blue(0.65f);
    colors[ImGuiCol_TextSelectedBg]       = blue(0.30f);
    colors[ImGuiCol_NavCursor]            = kGcGold;
}

// ---- Federation Gunship -------------------------------------------------------
// A handheld-console sci-fi look: chunky pixel fonts with black outlines, a deep
// navy tiled background, dialogue-box panels with pale borders, energy-tank
// gauges and a "map" of CPU cores lit room by room. Shapes sit on a 2-pixel grid
// (kPx) and the skin turns anti-aliasing off, so every edge stays crisp.

constexpr float kPx = 2.0f;   // one "console pixel"

const ImVec4 kFgNavy    (0.063f, 0.078f, 0.220f, 1.0f);   // background
const ImVec4 kFgTile    (0.090f, 0.115f, 0.300f, 1.0f);   // background tile edges
const ImVec4 kFgPanel   (0.035f, 0.060f, 0.150f, 1.0f);   // dialogue-box fill
const ImVec4 kFgFrame   (0.780f, 0.820f, 0.930f, 1.0f);   // pale borders
const ImVec4 kFgShade   (0.250f, 0.300f, 0.480f, 1.0f);   // inner borders, empty cells
const ImVec4 kFgMagenta (0.900f, 0.250f, 0.900f, 1.0f);   // title; normal load
const ImVec4 kFgYellow  (0.980f, 0.900f, 0.250f, 1.0f);   // labels; busy
const ImVec4 kFgRed     (1.000f, 0.250f, 0.330f, 1.0f);   // critical
const ImVec4 kFgCyan    (0.300f, 0.650f, 1.000f, 1.0f);   // "live", doors
const ImVec4 kFgGray    (0.560f, 0.570f, 0.620f, 1.0f);   // unlit map rooms
const ImVec4 kFgWhite   (0.970f, 0.970f, 1.000f, 1.0f);   // values

// Magenta when relaxed, yellow when busy, red when critical.
const ImVec4& gunshipLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kFgRed : (fraction > warnAt) ? kFgYellow : kFgMagenta;
}

float snapToPixel(float value) { return std::floor(value / kPx) * kPx; }
ImVec2 snapToPixel(const ImVec2& point) { return ImVec2(snapToPixel(point.x), snapToPixel(point.y)); }

// Text with an outline all round it (`outline` screen pixels thick), the way
// games keep text readable on any background.
void drawOutlinedText(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, const ImVec4& color, const std::string& text,
                      float outline = 1.0f, ImU32 outlineColor = IM_COL32(0, 0, 10, 255)) {
    const ImVec2 at(std::floor(pos.x), std::floor(pos.y));
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            if (dx != 0 || dy != 0) draw->AddText(font, size, at + ImVec2(dx * outline, dy * outline), outlineColor, text.c_str());
        }
    }
    draw->AddText(font, size, at, withAlpha(color, 1.0f), text.c_str());
}

// Outline of a rectangle, `thickness` thick, drawn inside it as four filled
// strips (no half-pixel lines). With `cutCorners`, the corner pixels are left
// out, which rounds the box the way 16-bit era menus do.
void drawPixelOutline(ImDrawList* draw, const ImVec2& a, const ImVec2& b, ImU32 color, float thickness, bool cutCorners) {
    const float cut = cutCorners ? thickness : 0.0f;
    draw->AddRectFilled(ImVec2(a.x + cut, a.y), ImVec2(b.x - cut, a.y + thickness), color);
    draw->AddRectFilled(ImVec2(a.x + cut, b.y - thickness), ImVec2(b.x - cut, b.y), color);
    draw->AddRectFilled(ImVec2(a.x, a.y + thickness), ImVec2(a.x + thickness, b.y - thickness), color);
    draw->AddRectFilled(ImVec2(b.x - thickness, a.y + thickness), ImVec2(b.x, b.y - thickness), color);
}

// A dialogue box: dark fill, a pale outer border with clipped corners and a
// thin darker border just inside it.
void drawPixelBox(ImDrawList* draw, const Box& box, float fillAlpha) {
    const ImVec2 a = snapToPixel(box.min), b = snapToPixel(box.max);
    draw->AddRectFilled(a + ImVec2(kPx, kPx), b - ImVec2(kPx, kPx), withAlpha(kFgPanel, fillAlpha));
    drawPixelOutline(draw, a, b, withAlpha(kFgFrame, 1.0f), kPx, true);
    drawPixelOutline(draw, a + ImVec2(kPx * 2, kPx * 2), b - ImVec2(kPx * 2, kPx * 2), withAlpha(kFgShade, 1.0f), kPx * 0.5f, false);
}

// Deep navy with a tiled grid (each tile has a darker bevel on two sides), like
// the walls of a ship's corridor.
void drawGunshipBackground(const ImVec2& size, const HudState&) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, withAlpha(kFgNavy, 1.0f));
    constexpr float kTile = 32.0f;
    for (float y = 0.0f; y < size.y; y += kTile) {
        for (float x = 0.0f; x < size.x; x += kTile) {
            if ((static_cast<int>(x / kTile) + static_cast<int>(y / kTile)) % 2 == 0) {
                draw->AddRectFilled(ImVec2(x, y), ImVec2(x + kTile, y + kTile), withAlpha(kFgTile, 0.35f));
            }
            draw->AddRectFilled(ImVec2(x, y + kTile - kPx), ImVec2(x + kTile, y + kTile), withAlpha(kFgTile, 1.0f));
            draw->AddRectFilled(ImVec2(x + kTile - kPx, y), ImVec2(x + kTile, y + kTile), withAlpha(kFgTile, 1.0f));
        }
    }
}

// A small down-arrow, like the "more text" prompt at the end of a dialogue
// box. Drawn as stacked pixel rows so it stays crisp.
void drawPixelArrow(ImDrawList* draw, const ImVec2& topLeft, ImU32 color) {
    const ImVec2 at = snapToPixel(topLeft);
    for (int row = 0; row < 4; ++row) {
        const float inset = kPx * static_cast<float>(row);
        draw->AddRectFilled(ImVec2(at.x + inset, at.y + kPx * row), ImVec2(at.x + kPx * 7 - inset, at.y + kPx * (row + 1)), color);
    }
}

// A dialogue-box panel with its title in yellow, a magenta marker before it and
// a dotted rule after it. While animating, a "more" arrow blinks in the
// bottom-right corner. Returns the area for content.
Box drawGunshipPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    drawPixelBox(draw, box, 0.90f);
    const ImVec2 a = snapToPixel(box.min), b = snapToPixel(box.max);
    const float titleY = a.y + 10.0f;
    draw->AddRectFilled(ImVec2(a.x + 10.0f, titleY), ImVec2(a.x + 18.0f, titleY + 8.0f), withAlpha(kFgMagenta, 1.0f));
    drawOutlinedText(draw, state.displayFont, 8.0f, ImVec2(a.x + 24.0f, titleY), kFgYellow, title);
    const float ruleLeft = a.x + 34.0f + textSize(state.displayFont, 8.0f, title).x;
    for (float x = snapToPixel(ruleLeft); x < b.x - 12.0f; x += kPx * 3) {
        draw->AddRectFilled(ImVec2(x, titleY + 4.0f), ImVec2(x + kPx, titleY + 4.0f + kPx), withAlpha(kFgFrame, 0.35f));
    }
    if (state.animate && fmodf(state.time, 1.0f) < 0.6f) drawPixelArrow(draw, ImVec2(b.x - 24.0f, b.y - 16.0f), withAlpha(kFgWhite, 0.9f));
    return Box{ImVec2(a.x + 12.0f, titleY + 18.0f), ImVec2(b.x - 12.0f, b.y - 12.0f)};
}

// "LABEL   value": a small yellow pixel label, the value in the pixel text font.
void drawGunshipInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.displayFont, 8.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);   // line the small label up with the taller value
    ImGui::TextColored(kFgYellow, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// A row of "energy tanks": `count` square cells, the first `lit` filled.
void drawEnergyTanks(ImDrawList* draw, const ImVec2& topLeft, int count, int lit, float cell, float gap, const ImVec4& color) {
    for (int i = 0; i < count; ++i) {
        const ImVec2 a = snapToPixel(topLeft + ImVec2((cell + gap) * static_cast<float>(i), 0.0f));
        const ImVec2 b = a + ImVec2(cell, cell);
        if (i < lit) {
            draw->AddRectFilled(a, b, withAlpha(color, 1.0f));
            draw->AddRectFilled(a + ImVec2(kPx, kPx), a + ImVec2(kPx * 2, kPx * 2), withAlpha(kFgWhite, 0.85f));   // a glint
        } else {
            draw->AddRectFilled(a, b, withAlpha(kFgPanel, 1.0f));
        }
        drawPixelOutline(draw, a, b, withAlpha(i < lit ? kFgFrame : kFgShade, 1.0f), kPx, false);
    }
}

// An energy readout per gauge, each in its own dialogue box: the label, a big
// pixel number, a row of ten energy tanks (one per 10%), a fine bar and the
// two readout lines.
void drawGunshipGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    constexpr float kGap = 12.0f;
    const float columnWidth = std::floor((box.width() - kGap * 3.0f) / 4.0f);
    for (int i = 0; i < 4; ++i) {
        const GaugeReading& reading = readings[i];
        const Box column{ImVec2(box.min.x + (columnWidth + kGap) * static_cast<float>(i), box.min.y),
                         ImVec2(box.min.x + (columnWidth + kGap) * static_cast<float>(i) + columnWidth, box.max.y)};
        drawPixelBox(draw, column, 0.88f);

        const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
        const ImVec4& color = gunshipLoadColor(percent, reading.warnAt, reading.critAt);
        const float centerX = std::floor((column.min.x + column.max.x) * 0.5f);
        const float contentHeight = 8.0f + 12.0f + 32.0f + 14.0f + 14.0f + 10.0f + 8.0f + 14.0f + 16.0f + 4.0f + 16.0f;
        float y = snapToPixel(column.min.y + std::max((column.height() - contentHeight) * 0.5f, 12.0f));

        drawOutlinedText(draw, state.displayFont, 8.0f, ImVec2(column.min.x + 12.0f, y), kFgYellow, reading.label);
        y += 8.0f + 12.0f;

        if (reading.hasValue) {
            const std::string number = format("%.0f", percent);
            const ImVec2 numberExtent = textSize(state.displayFont, 32.0f, number);
            const float left = centerX - (numberExtent.x + 18.0f) * 0.5f;
            drawOutlinedText(draw, state.displayFont, 32.0f, ImVec2(left, y), kFgWhite, number, kPx);
            drawOutlinedText(draw, state.displayFont, 16.0f, ImVec2(left + numberExtent.x + 2.0f, y + 16.0f), color, "%", kPx);
        } else {
            const ImVec2 naExtent = textSize(state.displayFont, 16.0f, "N/A");
            drawOutlinedText(draw, state.displayFont, 16.0f, ImVec2(centerX - naExtent.x * 0.5f, y + 8.0f), kFgGray, "N/A", kPx);
        }
        y += 32.0f + 14.0f;

        constexpr int kTanks = 10;
        constexpr float kTankGap = 4.0f;
        const float cell = std::clamp(snapToPixel((column.width() - 28.0f - kTankGap * (kTanks - 1)) / kTanks), 6.0f, 14.0f);
        const float tanksWidth = cell * kTanks + kTankGap * (kTanks - 1);
        const int lit = reading.hasValue ? static_cast<int>(std::lround(percent / 10.0f)) : 0;
        drawEnergyTanks(draw, ImVec2(centerX - tanksWidth * 0.5f, y), kTanks, lit, cell, kTankGap, color);
        y += 14.0f + 10.0f;

        const ImVec2 barA = snapToPixel(ImVec2(centerX - tanksWidth * 0.5f, y)), barB = snapToPixel(ImVec2(centerX + tanksWidth * 0.5f, y + 8.0f));
        draw->AddRectFilled(barA, barB, withAlpha(kFgPanel, 1.0f));
        if (reading.hasValue) {
            const float fillRight = snapToPixel(barA.x + kPx + (barB.x - barA.x - kPx * 2) * percent / 100.0f);
            draw->AddRectFilled(barA + ImVec2(kPx, kPx), ImVec2(fillRight, barB.y - kPx), withAlpha(color, 1.0f));
        }
        drawPixelOutline(draw, barA, barB, withAlpha(kFgShade, 1.0f), kPx, false);
        y += 8.0f + 14.0f;

        const ImVec2 line1Extent = textSize(state.textFont, 16.0f, reading.line1);
        drawOutlinedText(draw, state.textFont, 16.0f, ImVec2(centerX - line1Extent.x * 0.5f, y), kFgWhite, reading.line1);
        y += 16.0f + 4.0f;
        const ImVec2 line2Extent = textSize(state.textFont, 16.0f, reading.line2);
        drawOutlinedText(draw, state.textFont, 16.0f, ImVec2(centerX - line2Extent.x * 0.5f, y), kFgFrame, reading.line2);
    }
}

// The cores as rooms on a map: grey rooms that fill from the floor with the
// load colour, pale walls, and small blue doors between neighbours.
void drawGunshipCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawGunshipPanelFrame(draw, box, format("CORE MAP - %zu THREADS", state.cores.size()), state), "##GunshipCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = snapToPixel(ImGui::GetCursorScreenPos());
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kGap = 6.0f, kMinWidth = 62.0f, kMinHeight = 34.0f, kMaxHeight = 52.0f;
    const int count = static_cast<int>(state.cores.size());
    const int columns = std::clamp(static_cast<int>((room.x + kGap) / (kMinWidth + kGap)), 1, std::max(count, 1));
    const int rows = (count + columns - 1) / std::max(columns, 1);
    const float cellWidth = snapToPixel((room.x - kGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const float cellHeight = snapToPixel(std::clamp((room.y - kGap * static_cast<float>(rows - 1)) / static_cast<float>(std::max(rows, 1)),
                                                    kMinHeight, kMaxHeight));

    for (int i = 0; i < count; ++i) {
        const int column = i % columns, row = i / columns;
        const ImVec2 a = start + ImVec2((cellWidth + kGap) * static_cast<float>(column), (cellHeight + kGap) * static_cast<float>(row));
        const ImVec2 b = a + ImVec2(cellWidth, cellHeight);
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float floorY = snapToPixel(b.y - kPx - (cellHeight - kPx * 2) * value / 100.0f);

        panel->AddRectFilled(a, b, withAlpha(kFgGray, 0.85f));
        panel->AddRectFilled(ImVec2(a.x + kPx, floorY), b - ImVec2(kPx, kPx), withAlpha(gunshipLoadColor(value, 0.50f, 0.80f), 1.0f));
        drawPixelOutline(panel, a, b, withAlpha(kFgWhite, 0.95f), kPx, false);
        if (column + 1 < columns && i + 1 < count) {   // a door to the next room
            const float doorY = snapToPixel((a.y + b.y) * 0.5f - 4.0f);
            panel->AddRectFilled(ImVec2(b.x, doorY), ImVec2(b.x + kGap, doorY + 8.0f), withAlpha(kFgCyan, 1.0f));
        }

        drawOutlinedText(panel, state.displayFont, 8.0f, a + ImVec2(6.0f, 6.0f), kFgWhite, format("C%02d", state.coreId(i)));
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.textFont, 16.0f, text);
        drawOutlinedText(panel, state.textFont, 16.0f, ImVec2(b.x - extent.x - 6.0f, b.y - extent.y - 4.0f), kFgWhite, text);
    }
    ImGui::Dummy(ImVec2(room.x, (cellHeight + kGap) * static_cast<float>(rows) - kGap));
    ImGui::EndChild();
}

// Magenta pixel title; a "LIVE" legend with a blinking blue light, the clock and
// the gear on the right; a pale rule with notches underneath. Returns the buttons' rectangles.
HeaderButtons drawGunshipHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = snapToPixel((box.min.y + box.max.y) * 0.5f) - 4.0f;

    drawOutlinedText(draw, display, 16.0f, ImVec2(box.min.x, midY - 8.0f), kFgMagenta, "VEEASTATS", kPx);
    drawOutlinedText(draw, display, 8.0f, ImVec2(box.min.x + textSize(display, 16.0f, "VEEASTATS").x + 14.0f, midY - 2.0f), kFgYellow,
                  "SYSTEM STATUS");

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 18.0f;

    std::string clock, date;
    currentClock(clock, date);
    right -= textSize(display, 16.0f, clock).x;
    drawOutlinedText(draw, display, 16.0f, ImVec2(right, midY - 8.0f), kFgWhite, clock, kPx);
    right -= textSize(display, 8.0f, date).x + 14.0f;
    drawOutlinedText(draw, display, 8.0f, ImVec2(right, midY - 3.0f), kFgFrame, date);

    // "LIVE ■", like a map legend entry; the light blinks while animating.
    right -= 14.0f;
    const bool lightOn = !state.animate || fmodf(state.time, 1.0f) < 0.6f;
    draw->AddRectFilled(ImVec2(right, midY - 4.0f), ImVec2(right + 8.0f, midY + 4.0f), withAlpha(lightOn ? kFgCyan : kFgShade, 1.0f));
    drawPixelOutline(draw, ImVec2(right - kPx, midY - 4.0f - kPx), ImVec2(right + 8.0f + kPx, midY + 4.0f + kPx), IM_COL32(0, 0, 10, 255), kPx, false);
    right -= textSize(display, 8.0f, "LIVE").x + 8.0f;
    drawOutlinedText(draw, display, 8.0f, ImVec2(right, midY - 3.0f), kFgCyan, "LIVE");

    const float ruleY = snapToPixel(box.max.y) - kPx * 3;
    draw->AddRectFilled(ImVec2(box.min.x, ruleY), ImVec2(box.max.x, ruleY + kPx), withAlpha(kFgFrame, 0.9f));
    for (float x = snapToPixel(box.min.x); x < box.max.x; x += 16.0f) {
        draw->AddRectFilled(ImVec2(x, ruleY + kPx), ImVec2(x + kPx, ruleY + kPx * 3), withAlpha(kFgFrame, 0.6f));
    }
    return buttons;
}

void applyGunshipStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = 0.0f;
    style.WindowBorderSize = kPx;
    style.PopupBorderSize = kPx;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 10.0f;
    style.AntiAliasedLines = style.AntiAliasedFill = false;   // crisp pixel edges
    style.AntiAliasedLinesUseTex = false;

    ImVec4* colors = style.Colors;
    auto magenta = [](float alpha) { return ImVec4(kFgMagenta.x, kFgMagenta.y, kFgMagenta.z, alpha); };
    colors[ImGuiCol_Text]                 = kFgWhite;
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.66f, 0.70f, 0.86f, 1.0f);
    colors[ImGuiCol_WindowBg]             = ImVec4(kFgPanel.x, kFgPanel.y, kFgPanel.z, 0.97f);
    colors[ImGuiCol_PopupBg]              = ImVec4(kFgPanel.x, kFgPanel.y, kFgPanel.z, 0.97f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = kFgFrame;
    colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.16f, 0.38f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.20f, 0.50f, 1.0f);
    colors[ImGuiCol_FrameBgActive]        = magenta(0.55f);
    colors[ImGuiCol_Button]               = ImVec4(0.12f, 0.16f, 0.38f, 1.0f);
    colors[ImGuiCol_ButtonHovered]        = magenta(0.40f);
    colors[ImGuiCol_ButtonActive]         = magenta(0.60f);
    colors[ImGuiCol_Header]               = magenta(0.35f);
    colors[ImGuiCol_HeaderHovered]        = magenta(0.50f);
    colors[ImGuiCol_HeaderActive]         = magenta(0.65f);
    colors[ImGuiCol_CheckMark]            = kFgMagenta;
    colors[ImGuiCol_Separator]            = ImVec4(kFgFrame.x, kFgFrame.y, kFgFrame.z, 0.45f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = kFgShade;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(kFgFrame.x, kFgFrame.y, kFgFrame.z, 0.7f);
    colors[ImGuiCol_ScrollbarGrabActive]  = kFgFrame;
    colors[ImGuiCol_TextSelectedBg]       = magenta(0.35f);
    colors[ImGuiCol_NavCursor]            = kFgYellow;
}

// ---- Halloween ------------------------------------------------------------------
// A spooky game-menu look: a violet night with a big moon, bare trees and bats;
// slate-stone panels with thick outlines, title plaques and cobwebs; orange goo
// dripping under the header; jack-o'-lantern gauges that glow brighter as the
// load rises; and core tiles that fill with goo.
//
// Henny Penny's letters only fill about half of its nominal size (its tall
// swashes set the line height), so it is used about 1.4x larger than the other fonts.

const ImVec4 kHwNight   (0.075f, 0.035f, 0.130f, 1.0f);   // background, top
const ImVec4 kHwDusk    (0.190f, 0.075f, 0.290f, 1.0f);   // background, bottom
const ImVec4 kHwTree    (0.035f, 0.015f, 0.060f, 1.0f);   // tree and bat silhouettes
const ImVec4 kHwStone   (0.290f, 0.285f, 0.345f, 1.0f);   // panel fill
const ImVec4 kHwStoneLit(0.430f, 0.425f, 0.510f, 1.0f);   // panel highlights
const ImVec4 kHwOutline (0.075f, 0.045f, 0.105f, 1.0f);   // thick outlines
const ImVec4 kHwPlaque  (0.165f, 0.155f, 0.205f, 1.0f);   // title plaques, empty tracks
const ImVec4 kHwOrange  (1.000f, 0.560f, 0.120f, 1.0f);   // pumpkins, goo; normal load
const ImVec4 kHwGlow    (1.000f, 0.880f, 0.350f, 1.0f);   // candle light
const ImVec4 kHwPurple  (0.700f, 0.380f, 1.000f, 1.0f);   // busy
const ImVec4 kHwRed     (1.000f, 0.200f, 0.260f, 1.0f);   // critical
const ImVec4 kHwBone    (0.965f, 0.945f, 0.905f, 1.0f);   // text
const ImVec4 kHwMist    (0.780f, 0.720f, 0.880f, 1.0f);   // secondary text
const ImVec4 kHwWeb     (0.900f, 0.900f, 0.940f, 1.0f);   // cobwebs
const ImVec4 kHwStem    (0.340f, 0.520f, 0.180f, 1.0f);   // pumpkin stems

// Pumpkin orange when relaxed, witch purple when busy, red when critical.
const ImVec4& halloweenLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kHwRed : (fraction > warnAt) ? kHwPurple : kHwOrange;
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// A bare branch and its twigs, drawn recursively as tapering lines.
void drawBranch(ImDrawList* draw, const ImVec2& from, float angle, float length, float thickness, int depth, int seed) {
    const ImVec2 to = from + ImVec2(cosf(angle), sinf(angle)) * length;
    draw->AddLine(from, to, withAlpha(kHwTree, 1.0f), thickness);
    if (depth == 0) return;
    for (int i = 0; i < 2; ++i) {
        const float spread = 0.35f + 0.4f * hash01(seed * 7 + i);
        drawBranch(draw, to, angle + (i == 0 ? -spread : spread), length * (0.62f + 0.18f * hash01(seed * 13 + i)), thickness * 0.65f,
                   depth - 1, seed * 3 + i + 1);
    }
}

// A bat: a small body and two pointed wings that flap.
void drawBat(ImDrawList* draw, const ImVec2& center, float size, float flap) {
    const ImU32 color = withAlpha(kHwTree, 1.0f);
    const float lift = size * 0.45f * flap;
    for (const float side : {-1.0f, 1.0f}) {
        const ImVec2 wing[] = {center, center + ImVec2(side * size * 0.55f, -size * 0.25f - lift),
                               center + ImVec2(side * size * 1.1f, -size * 0.05f - lift), center + ImVec2(side * size * 0.75f, size * 0.12f),
                               center + ImVec2(side * size * 0.45f, size * 0.02f)};
        draw->AddConcavePolyFilled(wing, 5, color);
    }
    draw->AddCircleFilled(center, size * 0.2f, color);
}

// Violet night: a big moon with a soft glow, twinkling stars, bare trees in the
// bottom corners, and (while animating) a few bats drifting across.
void drawHalloweenBackground(const ImVec2& size, const HudState& state) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 top = withAlpha(kHwNight, 1.0f), bottom = withAlpha(kHwDusk, 1.0f);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, top, top, bottom, bottom);

    for (int i = 0; i < 70; ++i) {
        const ImVec2 star(hash01(i * 3) * size.x, hash01(i * 3 + 1) * size.y * 0.75f);
        const float twinkle = state.animate ? 0.6f + 0.4f * sinf(state.time * (1.0f + hash01(i)) * 2.0f + static_cast<float>(i)) : 0.8f;
        draw->AddCircleFilled(star, 0.8f + hash01(i * 3 + 2) * 1.0f, withAlpha(kHwBone, 0.55f * twinkle));
    }

    const ImVec2 moon(size.x * 0.84f, size.y * 0.16f);
    const float moonRadius = std::min(size.x, size.y) * 0.09f;
    for (int i = 3; i >= 1; --i) draw->AddCircleFilled(moon, moonRadius * (1.0f + 0.35f * static_cast<float>(i)), withAlpha(kHwGlow, 0.035f));
    draw->AddCircleFilled(moon, moonRadius, IM_COL32(255, 244, 214, 235));
    draw->AddCircleFilled(moon + ImVec2(-moonRadius * 0.3f, -moonRadius * 0.2f), moonRadius * 0.18f, IM_COL32(220, 205, 175, 160));
    draw->AddCircleFilled(moon + ImVec2(moonRadius * 0.35f, moonRadius * 0.25f), moonRadius * 0.12f, IM_COL32(220, 205, 175, 160));

    const float treeHeight = size.y * 0.22f;
    drawBranch(draw, ImVec2(size.x * 0.03f, size.y), -kPi * 0.45f, treeHeight, 9.0f, 5, 1);
    drawBranch(draw, ImVec2(size.x * 0.97f, size.y), -kPi * 0.56f, treeHeight * 0.9f, 8.0f, 5, 2);

    if (state.animate) {
        for (int i = 0; i < 3; ++i) {
            const float speed = 18.0f + 8.0f * static_cast<float>(i);
            const float x = fmodf(state.time * speed + static_cast<float>(i) * 310.0f, size.x + 120.0f) - 60.0f;
            const float y = size.y * (0.10f + 0.07f * static_cast<float>(i)) + sinf(state.time * 1.3f + static_cast<float>(i)) * 10.0f;
            drawBat(draw, ImVec2(x, y), 11.0f - static_cast<float>(i) * 2.0f, sinf(state.time * 9.0f + static_cast<float>(i) * 2.0f));
        }
    }
}

// A slate-stone slab: a dark outline round a rounded stone face that is a
// little lighter at the top, a thin highlight just inside the edge, and a few
// faint specks so it doesn't look flat.
void drawStoneSlab(ImDrawList* draw, const Box& box, float rounding, float alpha) {
    constexpr float kOutline = 3.0f;
    draw->AddRectFilled(box.min + ImVec2(2.0f, 4.0f), box.max + ImVec2(2.0f, 4.0f), IM_COL32(0, 0, 0, static_cast<int>(70 * alpha)), rounding + kOutline);
    draw->AddRectFilled(box.min - ImVec2(kOutline, kOutline), box.max + ImVec2(kOutline, kOutline), withAlpha(kHwOutline, alpha),
                        rounding + kOutline);
    draw->AddRectFilled(box.min, box.max, withAlpha(kHwStone, alpha), rounding);
    draw->AddRectFilled(box.min, ImVec2(box.max.x, box.min.y + std::min(box.height() * 0.45f, 60.0f)), withAlpha(kHwStoneLit, 0.25f * alpha),
                        rounding, ImDrawFlags_RoundCornersTop);
    draw->AddRect(box.min + ImVec2(3.0f, 3.0f), box.max - ImVec2(3.0f, 3.0f), withAlpha(kHwStoneLit, 0.45f * alpha), std::max(rounding - 3.0f, 0.0f),
                  0, 1.5f);
    const int seed = static_cast<int>(box.min.x * 7.0f + box.min.y * 13.0f);
    for (int i = 0; i < static_cast<int>(box.width() * box.height() / 2500.0f); ++i) {
        const ImVec2 speck(box.min.x + 8.0f + hash01(seed + i * 2) * (box.width() - 16.0f),
                           box.min.y + 8.0f + hash01(seed + i * 2 + 1) * (box.height() - 16.0f));
        draw->AddCircleFilled(speck, 1.0f + 2.0f * hash01(seed + i), withAlpha(kHwOutline, 0.12f * alpha));
    }
}

// A cobweb spanning a corner: threads fanning out from `corner`, joined by
// sagging strands. `xSign`/`ySign` (+1 or -1) say which way the web opens.
void drawCobweb(ImDrawList* draw, const ImVec2& corner, float size, float xSign, float ySign, float alpha) {
    const ImU32 color = withAlpha(kHwWeb, alpha);
    constexpr int kThreads = 5;
    ImVec2 ends[kThreads];
    for (int i = 0; i < kThreads; ++i) {
        const float angle = kPi * 0.5f * static_cast<float>(i) / (kThreads - 1);
        ends[i] = ImVec2(cosf(angle) * xSign, sinf(angle) * ySign);
        draw->AddLine(corner, corner + ends[i] * size, color, 1.0f);
    }
    for (const float ring : {0.35f, 0.62f, 0.9f}) {
        for (int i = 0; i + 1 < kThreads; ++i) {
            const ImVec2 a = corner + ends[i] * size * ring, b = corner + ends[i + 1] * size * ring;
            draw->AddBezierQuadratic(a, (a + b) * 0.5f + (corner - (a + b) * 0.5f) * 0.18f, b, color, 1.0f);   // sags towards the corner
        }
    }
}

// Text with a dark outline all round, like game-menu titles.
void drawHalloweenText(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, const ImVec4& color, const std::string& text,
                       float outline = 2.0f) {
    drawOutlinedText(draw, font, size, pos, color, text, outline, withAlpha(kHwOutline, 1.0f));
}

// A dark rounded plaque centred on `centerX`, its top at `top`, holding `text`.
void drawPlaque(ImDrawList* draw, ImFont* font, float size, float centerX, float top, const std::string& text) {
    const ImVec2 extent = textSize(font, size, text);
    const Box plaque{ImVec2(std::floor(centerX - extent.x * 0.5f - 14.0f), top), ImVec2(std::floor(centerX + extent.x * 0.5f + 14.0f), top + extent.y + 6.0f)};
    draw->AddRectFilled(plaque.min - ImVec2(2.0f, 2.0f), plaque.max + ImVec2(2.0f, 2.0f), withAlpha(kHwOutline, 1.0f), 9.0f);
    draw->AddRectFilled(plaque.min, plaque.max, withAlpha(kHwPlaque, 1.0f), 7.0f);
    draw->AddLine(ImVec2(plaque.min.x + 6.0f, plaque.min.y + 2.0f), ImVec2(plaque.max.x - 6.0f, plaque.min.y + 2.0f), withAlpha(kHwStoneLit, 0.6f), 1.0f);
    drawHalloweenText(draw, font, size, ImVec2(centerX - extent.x * 0.5f, top + 3.0f), kHwBone, text, 1.5f);
}

// A band of orange goo with drips hanging from it. While animating, the drips
// slowly stretch and shrink.
void drawGooBand(ImDrawList* draw, const Box& band, const HudState& state) {
    draw->AddRectFilled(band.min - ImVec2(0.0f, 2.0f), band.max + ImVec2(0.0f, 2.0f), withAlpha(kHwOutline, 1.0f), band.height() * 0.5f + 2.0f);
    draw->AddRectFilled(band.min, band.max, withAlpha(kHwOrange, 1.0f), band.height() * 0.5f);
    for (float x = band.min.x + 14.0f; x < band.max.x - 14.0f; x += 23.0f) {
        const int i = static_cast<int>(x);
        const float width = 5.0f + 4.0f * hash01(i);
        const float wobble = state.animate ? 2.5f * sinf(state.time * 1.5f + hash01(i + 1) * 6.0f) : 0.0f;
        const float length = 3.0f + 13.0f * hash01(i + 2) + wobble;
        const ImVec2 a(x - width * 0.5f, band.max.y - 2.0f), b(x + width * 0.5f, band.max.y + length);
        draw->AddRectFilled(a - ImVec2(1.5f, 0.0f), b + ImVec2(1.5f, 1.5f), withAlpha(kHwOutline, 1.0f), width * 0.5f + 1.5f,
                            ImDrawFlags_RoundCornersBottom);
        draw->AddRectFilled(a, b, withAlpha(kHwOrange, 1.0f), width * 0.5f, ImDrawFlags_RoundCornersBottom);
    }
    draw->AddLine(ImVec2(band.min.x + band.height(), band.min.y + 2.0f), ImVec2(band.max.x - band.height(), band.min.y + 2.0f),
                  withAlpha(kHwGlow, 0.7f), 1.5f);   // a wet highlight along the top
}

// A jack-o'-lantern whose carved face glows brighter as `glow` (0..1) rises.
// While animating, the candle inside flickers.
void drawJackOLantern(ImDrawList* draw, const ImVec2& center, float radius, float glow, const HudState& state, int seed) {
    if (state.animate) glow *= 0.88f + 0.12f * sinf(state.time * 13.0f + static_cast<float>(seed)) * sinf(state.time * 7.0f + static_cast<float>(seed) * 2.0f);
    glow = std::clamp(glow, 0.12f, 1.0f);

    // Halo, then the body: an outline, then five overlapping ribs, darkest at the sides.
    for (int i = 3; i >= 1; --i) draw->AddCircleFilled(center, radius * (1.0f + 0.22f * static_cast<float>(i)), withAlpha(kHwOrange, 0.05f * glow));
    const ImVec2 ribs[] = {{-0.48f, 0.0f}, {0.48f, 0.0f}, {-0.24f, 0.0f}, {0.24f, 0.0f}, {0.0f, 0.0f}};
    const float ribWidths[] = {0.52f, 0.52f, 0.55f, 0.55f, 0.55f};
    const float ribShades[] = {0.62f, 0.62f, 0.80f, 0.80f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        draw->AddEllipseFilled(center + ribs[i] * radius, ImVec2(ribWidths[i] * radius + 2.5f, 0.82f * radius + 2.5f), withAlpha(kHwOutline, 1.0f));
    }
    for (int i = 0; i < 5; ++i) {
        const ImVec4 shade = mix(ImVec4(0.55f, 0.20f, 0.04f, 1.0f), kHwOrange, ribShades[i]);
        draw->AddEllipseFilled(center + ribs[i] * radius, ImVec2(ribWidths[i] * radius, 0.82f * radius), withAlpha(shade, 1.0f));
    }
    // Stem.
    const ImVec2 stemBase = center + ImVec2(0.0f, -0.78f * radius);
    const ImVec2 stem[] = {stemBase + ImVec2(-0.09f, 0.05f) * radius, stemBase + ImVec2(-0.05f, -0.30f) * radius,
                           stemBase + ImVec2(0.14f, -0.36f) * radius, stemBase + ImVec2(0.09f, 0.05f) * radius};
    draw->AddConvexPolyFilled(stem, 4, withAlpha(kHwStem, 1.0f));
    draw->AddPolyline(stem, 4, withAlpha(kHwOutline, 1.0f), ImDrawFlags_Closed, 2.0f);

    // The carved face, lit from inside.
    const ImU32 lit = withAlpha(mix(ImVec4(0.30f, 0.09f, 0.02f, 1.0f), kHwGlow, glow), 1.0f);
    for (const float side : {-1.0f, 1.0f}) {
        draw->AddTriangleFilled(center + ImVec2(side * 0.40f, -0.08f) * radius, center + ImVec2(side * 0.12f, -0.08f) * radius,
                                center + ImVec2(side * 0.26f, -0.42f) * radius, lit);
    }
    draw->AddTriangleFilled(center + ImVec2(-0.07f, 0.10f) * radius, center + ImVec2(0.07f, 0.10f) * radius, center + ImVec2(0.0f, -0.03f) * radius, lit);
    const ImVec2 mouth[] = {center + ImVec2(-0.52f, 0.20f) * radius, center + ImVec2(-0.30f, 0.30f) * radius, center + ImVec2(-0.22f, 0.20f) * radius,
                            center + ImVec2(-0.10f, 0.32f) * radius, center + ImVec2(0.06f, 0.22f) * radius,  center + ImVec2(0.20f, 0.32f) * radius,
                            center + ImVec2(0.30f, 0.21f) * radius,  center + ImVec2(0.52f, 0.20f) * radius,  center + ImVec2(0.30f, 0.52f) * radius,
                            center + ImVec2(-0.30f, 0.52f) * radius};
    draw->AddConcavePolyFilled(mouth, 10, lit);
}

// A chunky arc: an outlined dark track with the value filled in over it, with
// rounded ends and a glossy highlight, like the game's progress bars.
void drawGooArc(ImDrawList* draw, const ImVec2& center, float radius, float thickness, float from, float sweep, float fraction, const ImVec4& color) {
    const auto stroke = [&](float a, float b, ImU32 c, float t) {
        draw->PathArcTo(center, radius, a, b, 48);
        draw->PathStroke(c, 0, t);
        draw->AddCircleFilled(pointOnCircle(center, radius, a), t * 0.5f, c);
        draw->AddCircleFilled(pointOnCircle(center, radius, b), t * 0.5f, c);
    };
    stroke(from, from + sweep, withAlpha(kHwOutline, 1.0f), thickness + 6.0f);
    stroke(from, from + sweep, withAlpha(kHwPlaque, 1.0f), thickness);
    if (fraction > 0.005f) {
        const float to = from + sweep * std::clamp(fraction, 0.0f, 1.0f);
        stroke(from, to, withAlpha(color, 1.0f), thickness);
        draw->PathArcTo(center, radius + thickness * 0.22f, from, to, 48);
        draw->PathStroke(withAlpha(kHwBone, 0.35f), 0, thickness * 0.22f);
    }
}

// A gauge: a jack-o'-lantern inside a chunky 270-degree ring, the label on a
// plaque at the top, the number in the ring's opening and the readouts below.
void drawHalloweenGauge(ImDrawList* draw, const ImVec2& center, float radius, const GaugeReading& reading, const HudState& state, int seed) {
    constexpr float kStart = kPi * 0.75f, kSweep = kPi * 1.5f;
    const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
    const ImVec4& color = halloweenLoadColor(percent, reading.warnAt, reading.critAt);
    const float thickness = std::max(radius * 0.15f, 7.0f);
    drawGooArc(draw, center, radius - thickness * 0.5f - 3.0f, thickness, kStart, kSweep, reading.hasValue ? percent / 100.0f : 0.0f, color);
    drawJackOLantern(draw, center + ImVec2(0.0f, -radius * 0.04f), radius * 0.50f, reading.hasValue ? percent / 100.0f : 0.0f, state, seed);

    const float numberSize = std::round(std::max(radius * 0.42f, 22.0f));
    const std::string number = reading.hasValue ? format("%.0f%%", percent) : "N/A";
    const ImVec2 numberExtent = textSize(state.displayFont, numberSize, number);
    drawHalloweenText(draw, state.displayFont, numberSize, ImVec2(center.x - numberExtent.x * 0.5f, center.y + radius * 0.80f - numberExtent.y * 0.5f),
                      reading.hasValue ? kHwBone : kHwMist, number);
    drawPlaque(draw, state.displayFont, 19.0f, center.x, center.y - radius - 10.0f, reading.label);

    // Both readouts on one line under the dial, "4.32 GHz · VCORE 1.208 V".
    const std::string readout = reading.line1.empty() ? reading.line2 : reading.line1 + "  ·  " + reading.line2;
    const ImVec2 readoutExtent = textSize(state.textFont, 14.0f, readout);
    drawHalloweenText(draw, state.textFont, 14.0f, ImVec2(center.x - readoutExtent.x * 0.5f, center.y + radius + 8.0f), kHwMist, readout, 1.0f);
}

void drawHalloweenGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    const DialLayout layout = dialLayout(box);
    const float radius = std::floor(layout.radius * 0.84f);   // room for the plaque above and the readout below
    for (int i = 0; i < 4; ++i) {
        drawHalloweenGauge(draw, ImVec2(layout.centerX(box, i), layout.centerY + 4.0f), radius, readings[i], state, i);
    }
}

// A stone panel with cobwebs in its top corners and the title on a plaque
// sitting on its top edge. Returns the area for content.
Box drawHalloweenPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    const Box slab{box.min + ImVec2(3.0f, 8.0f), box.max - ImVec2(3.0f, 3.0f)};
    drawStoneSlab(draw, slab, 14.0f, 1.0f);
    drawCobweb(draw, slab.min + ImVec2(4.0f, 4.0f), 34.0f, 1.0f, 1.0f, 0.55f);
    drawCobweb(draw, ImVec2(slab.max.x - 4.0f, slab.min.y + 4.0f), 34.0f, -1.0f, 1.0f, 0.55f);
    drawPlaque(draw, state.displayFont, 19.0f, std::floor((slab.min.x + slab.max.x) * 0.5f), box.min.y - 2.0f, title);
    return Box{ImVec2(slab.min.x + 16.0f, slab.min.y + 26.0f), ImVec2(slab.max.x - 12.0f, slab.max.y - 12.0f)};
}

// "LABEL   value": a small orange label, the value in white.
void drawHalloweenInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.textFont, 13.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);   // line the small label up with the taller value
    ImGui::TextColored(kHwOrange, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// The cores as stone tiles that fill with goo from the bottom; the surface of
// the goo ripples while animating.
void drawHalloweenCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawHalloweenPanelFrame(draw, box, format("Cores (%zu threads)", state.cores.size()), state), "##HalloweenCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2(2.0f, 2.0f);
    const ImVec2 room = ImGui::GetContentRegionAvail() - ImVec2(4.0f, 4.0f);

    constexpr float kGap = 9.0f, kMinWidth = 56.0f, kMinHeight = 34.0f, kMaxHeight = 52.0f;
    const int count = static_cast<int>(state.cores.size());
    const int columns = std::clamp(static_cast<int>((room.x + kGap) / (kMinWidth + kGap)), 1, std::max(count, 1));
    const int rows = (count + columns - 1) / std::max(columns, 1);
    const float cellWidth = std::floor((room.x - kGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const float cellHeight = std::floor(std::clamp((room.y - kGap * static_cast<float>(rows - 1)) / static_cast<float>(std::max(rows, 1)), kMinHeight, kMaxHeight));

    for (int i = 0; i < count; ++i) {
        const ImVec2 a = start + ImVec2((cellWidth + kGap) * static_cast<float>(i % columns), (cellHeight + kGap) * static_cast<float>(i / columns));
        const ImVec2 b = a + ImVec2(cellWidth, cellHeight);
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const ImVec4& color = halloweenLoadColor(value, 0.50f, 0.80f);

        panel->AddRectFilled(a - ImVec2(2.5f, 2.5f), b + ImVec2(2.5f, 2.5f), withAlpha(kHwOutline, 1.0f), 9.0f);
        panel->AddRectFilled(a, b, withAlpha(kHwPlaque, 1.0f), 7.0f);
        const float surface = b.y - (cellHeight - 4.0f) * value / 100.0f - 2.0f;
        if (value > 0.5f) {
            panel->PushClipRect(a, b, true);
            panel->AddRectFilled(ImVec2(a.x, surface + 2.0f), b, withAlpha(color, 1.0f), 7.0f, ImDrawFlags_RoundCornersBottom);
            const float phase = state.animate ? state.time * 2.0f + static_cast<float>(i) : 0.0f;
            for (float x = a.x + 3.0f; x < b.x; x += 7.0f) {   // ripples
                panel->AddCircleFilled(ImVec2(x, surface + 2.0f + 1.5f * sinf(phase + x * 0.4f)), 3.5f, withAlpha(color, 1.0f));
            }
            panel->PopClipRect();
        }
        panel->AddLine(ImVec2(a.x + 6.0f, a.y + 2.0f), ImVec2(b.x - 6.0f, a.y + 2.0f), withAlpha(kHwStoneLit, 0.7f), 1.0f);

        drawHalloweenText(panel, state.textFont, 12.0f, a + ImVec2(6.0f, 3.0f), kHwMist, format("C%02d", state.coreId(i)), 1.0f);
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.displayFont, 19.0f, text);
        drawHalloweenText(panel, state.displayFont, 19.0f, ImVec2(b.x - extent.x - 5.0f, b.y - extent.y + 1.0f), kHwBone, text, 1.5f);
    }
    ImGui::Dummy(ImVec2(room.x, (cellHeight + kGap) * static_cast<float>(rows)));
    ImGui::EndChild();
}

// A flickering candle flame: a teardrop of orange with a yellow core.
void drawCandleFlame(ImDrawList* draw, const ImVec2& base, float height, const HudState& state) {
    const float flicker = state.animate ? 0.85f + 0.15f * sinf(state.time * 17.0f) * sinf(state.time * 5.0f) : 1.0f;
    const float h = height * flicker, w = height * 0.38f;
    for (const auto& [scale, color] : {std::pair{1.0f, kHwOrange}, std::pair{0.55f, kHwGlow}}) {
        draw->AddCircleFilled(base + ImVec2(0.0f, -w * scale), w * scale, withAlpha(color, 1.0f));
        draw->AddTriangleFilled(base + ImVec2(-w * scale * 0.95f, -w * scale), base + ImVec2(w * scale * 0.95f, -w * scale),
                                base + ImVec2(0.0f, -h * scale), withAlpha(color, 1.0f));
    }
}

// The spooky title, a candle-lit "LIVE", the clock and the gear; goo dripping
// along the bottom. Returns the buttons' rectangles.
HeaderButtons drawHalloweenHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 5.0f;
    const ImVec2 titleExtent = textSize(state.displayFont, 34.0f, "VeeaStats");
    drawHalloweenText(draw, state.displayFont, 34.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), kHwOrange, "VeeaStats", 2.5f);
    const ImVec2 subtitleExtent = textSize(state.textFont, 13.0f, "Haunted Hardware");
    drawHalloweenText(draw, state.textFont, 13.0f, ImVec2(box.min.x + titleExtent.x + 14.0f, midY - subtitleExtent.y * 0.5f + 3.0f), kHwMist,
                      "Haunted Hardware", 1.0f);

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 18.0f;

    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(state.displayFont, 26.0f, clock);
    right -= clockExtent.x;
    drawHalloweenText(draw, state.displayFont, 26.0f, ImVec2(right, midY - clockExtent.y * 0.5f), kHwBone, clock);
    const ImVec2 dateExtent = textSize(state.textFont, 12.0f, date);
    right -= dateExtent.x + 12.0f;
    drawHalloweenText(draw, state.textFont, 12.0f, ImVec2(right, midY - dateExtent.y * 0.5f + 1.0f), kHwMist, date, 1.0f);

    const ImVec2 liveExtent = textSize(state.textFont, 12.0f, "LIVE");
    right -= liveExtent.x + 22.0f;
    drawCandleFlame(draw, ImVec2(right - 6.0f, midY + 6.0f), 14.0f, state);
    drawHalloweenText(draw, state.textFont, 12.0f, ImVec2(right + 4.0f, midY - liveExtent.y * 0.5f + 1.0f), kHwOrange, "LIVE", 1.0f);

    drawGooBand(draw, Box{ImVec2(box.min.x, box.max.y - 7.0f), ImVec2(box.max.x, box.max.y - 1.0f)}, state);
    return buttons;
}

void applyHalloweenStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.WindowRounding = style.PopupRounding = 10.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 8.0f;
    style.ScrollbarRounding = style.GrabRounding = 6.0f;
    style.WindowBorderSize = style.PopupBorderSize = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 10.0f;

    ImVec4* colors = style.Colors;
    auto orange = [](float alpha) { return ImVec4(kHwOrange.x, kHwOrange.y, kHwOrange.z, alpha); };
    auto purple = [](float alpha) { return ImVec4(kHwPurple.x, kHwPurple.y, kHwPurple.z, alpha); };
    colors[ImGuiCol_Text]                 = kHwBone;
    colors[ImGuiCol_TextDisabled]         = kHwMist;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.20f, 0.19f, 0.25f, 0.98f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.20f, 0.19f, 0.25f, 0.98f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = ImVec4(0.08f, 0.05f, 0.11f, 1.0f);
    colors[ImGuiCol_FrameBg]              = kHwPlaque;
    colors[ImGuiCol_FrameBgHovered]       = purple(0.40f);
    colors[ImGuiCol_FrameBgActive]        = purple(0.60f);
    colors[ImGuiCol_Button]               = orange(0.80f);
    colors[ImGuiCol_ButtonHovered]        = orange(0.95f);
    colors[ImGuiCol_ButtonActive]         = kHwGlow;
    colors[ImGuiCol_Header]               = orange(0.55f);
    colors[ImGuiCol_HeaderHovered]        = orange(0.75f);
    colors[ImGuiCol_HeaderActive]         = orange(0.90f);
    colors[ImGuiCol_CheckMark]            = kHwOrange;
    colors[ImGuiCol_Separator]            = ImVec4(kHwMist.x, kHwMist.y, kHwMist.z, 0.30f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = kHwStoneLit;
    colors[ImGuiCol_ScrollbarGrabHovered] = orange(0.70f);
    colors[ImGuiCol_ScrollbarGrabActive]  = kHwOrange;
    colors[ImGuiCol_TextSelectedBg]       = purple(0.40f);
    colors[ImGuiCol_NavCursor]            = kHwOrange;
}

// ---- Poseidon -------------------------------------------------------------------
// The sea god's temple: deep stormy blues, gold trim with Greek key borders,
// carved Roman capitals and marble-white numbers. The gauges are round bronze
// shields with a trident pointing at the value, the cores a colonnade of
// temple columns filling with sea water, and lightning forks across the sky.

const ImVec4 kPoGold      (0.86f, 0.68f, 0.32f, 1.0f);    // trim, titles, labels; busy
const ImVec4 kPoMarble    (0.93f, 0.95f, 0.97f, 1.0f);    // values
const ImVec4 kPoFoam      (0.62f, 0.78f, 0.86f, 1.0f);    // quiet text
const ImVec4 kPoSea       (0.22f, 0.74f, 0.84f, 1.0f);    // water; relaxed
const ImVec4 kPoBolt      (0.74f, 0.93f, 1.00f, 1.0f);    // lightning
const ImVec4 kPoClay      (0.90f, 0.42f, 0.22f, 1.0f);    // critical: the terracotta of Greek pottery
const ImVec4 kPoPanel     (0.03f, 0.09f, 0.16f, 1.0f);    // panel fill
const ImVec4 kPoDeepTop   (0.05f, 0.14f, 0.23f, 1.0f);    // background, top...
const ImVec4 kPoDeepBottom(0.01f, 0.04f, 0.08f, 1.0f);    // ...and bottom

// Sea blue when relaxed, gold when busy, terracotta when critical.
const ImVec4& poseidonLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kPoClay : (fraction > warnAt) ? kPoGold : kPoSea;
}

// A Greek key (meander) band from x0 to x1, as painted round the rim of Greek
// pottery: square hooks of `height` on a shared baseline.
void drawGreekKey(ImDrawList* draw, float x0, float x1, float top, float height, ImU32 color) {
    const float bottom = std::floor(top + height) + 0.5f;
    top = std::floor(top) + 0.5f;
    draw->AddLine(ImVec2(x0, bottom), ImVec2(x1, bottom), color);
    for (float x = std::floor(x0) + 0.5f; x + height <= x1; x += height) {
        const ImVec2 hook[] = {ImVec2(x, bottom), ImVec2(x, top), ImVec2(std::floor(x + height * 0.78f), top),
                               ImVec2(std::floor(x + height * 0.78f), std::floor(top + height * 0.62f)),
                               ImVec2(std::floor(x + height * 0.39f), std::floor(top + height * 0.62f)),
                               ImVec2(std::floor(x + height * 0.39f), std::floor(top + height * 0.32f))};
        draw->AddPolyline(hook, 6, color, ImDrawFlags_None, 1.0f);
    }
}

// A jagged path from `a` to `b` (appended to `points`, without `a`), made by
// midpoint displacement: each stretch is halved and its middle pushed sideways
// by a random share of its length, then each half again, so the line zig-zags
// at every scale the way real lightning does. `seed` advances as it is used.
void lightningPath(std::vector<ImVec2>& points, const ImVec2& a, const ImVec2& b, int depth, int& seed) {
    if (depth == 0) {
        points.push_back(b);
        return;
    }
    const ImVec2 along = b - a;
    const ImVec2 sideways(-along.y, along.x);   // as long as `along`
    const ImVec2 middle = (a + b) * 0.5f + sideways * ((hash01(seed++) - 0.5f) * 0.45f);
    lightningPath(points, a, middle, depth - 1, seed);
    lightningPath(points, middle, b, depth - 1, seed);
}

// Strokes a bolt's path, thinning towards its end: a wide faint blue glow, a
// brighter blue body, and a white-hot core.
void strokeBolt(ImDrawList* draw, const std::vector<ImVec2>& points, float width, float alpha) {
    const size_t segments = points.size() - 1;
    for (size_t i = 0; i < segments; ++i) {
        const float taper = 1.0f - 0.65f * static_cast<float>(i) / static_cast<float>(segments);
        const float w = width * taper;
        draw->AddLine(points[i], points[i + 1], withAlpha(kPoBolt, 0.05f * alpha), w * 9.0f);
        draw->AddLine(points[i], points[i + 1], withAlpha(kPoBolt, 0.14f * alpha), w * 4.0f);
        draw->AddLine(points[i], points[i + 1], withAlpha(kPoBolt, 0.55f * alpha), w * 2.0f);
        draw->AddLine(points[i], points[i + 1], withAlpha(ImVec4(1, 1, 1, 1), alpha), w);
    }
}

// A lightning bolt from `from` towards `to`, with a few thinner forks
// splitting off it, and a glow where it starts. `seed` picks its shape.
void drawLightning(ImDrawList* draw, const ImVec2& from, const ImVec2& to, int seed, float alpha, float width = 2.2f) {
    int next = seed * 977;
    std::vector<ImVec2> bolt = {from};
    lightningPath(bolt, from, to, 6, next);   // 64 segments

    for (int glow = 3; glow >= 1; --glow) {   // the lit cloud it comes out of
        draw->AddCircleFilled(from, width * 9.0f * static_cast<float>(glow), withAlpha(kPoBolt, 0.035f * alpha));
    }
    const ImVec2 along = to - from;
    for (int fork = 0; fork < 3; ++fork) {
        const size_t at = 10 + static_cast<size_t>(hash01(next++) * 36.0f);   // where along the bolt it splits off
        const float side = hash01(next++) < 0.5f ? -1.0f : 1.0f;
        const float turn = side * (0.35f + 0.5f * hash01(next++));
        const float length = 0.18f + 0.20f * hash01(next++);
        const ImVec2 direction(along.x * cosf(turn) - along.y * sinf(turn), along.x * sinf(turn) + along.y * cosf(turn));
        std::vector<ImVec2> branch = {bolt[at]};
        lightningPath(branch, bolt[at], bolt[at] + direction * length, 4, next);
        strokeBolt(draw, branch, width * 0.45f, alpha * 0.8f);
    }
    strokeBolt(draw, bolt, width, alpha);
}

// A stormy deep-sea gradient, slow swells along the bottom, a faint bolt far
// off in the corner and, while animating, a bolt that strikes every few seconds.
void drawPoseidonBackground(const ImVec2& size, const HudState& state) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 top = withAlpha(kPoDeepTop, 1.0f), bottom = withAlpha(kPoDeepBottom, 1.0f);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, top, top, bottom, bottom);

    for (int row = 0; row < 4; ++row) {
        const float baseY = size.y - 16.0f - static_cast<float>(row) * 24.0f;
        for (float x = 0.0f; x <= size.x + 8.0f; x += 8.0f) {
            draw->PathLineTo(ImVec2(x, baseY + sinf(x * 0.017f + state.time * 0.7f + static_cast<float>(row) * 1.9f) * 5.0f));
        }
        draw->PathStroke(withAlpha(kPoSea, 0.10f - 0.02f * static_cast<float>(row)), 1.5f);
    }

    drawLightning(draw, ImVec2(size.x * 0.99f, 56.0f), ImVec2(size.x * 0.955f, size.y * 0.58f), 11, 0.55f, 1.6f);   // far off, always there (below the header)
    if (state.animate) {
        constexpr float kEvery = 9.0f;
        const float cycle = fmodf(state.time, kEvery);
        if (cycle < 0.40f) {
            const int strike = static_cast<int>(state.time / kEvery);
            const float alpha = cycle < 0.06f ? 1.0f : cycle < 0.12f ? 0.25f : cycle < 0.20f ? 0.9f : (0.40f - cycle) / 0.20f * 0.6f;
            draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, withAlpha(kPoBolt, 0.035f * alpha));
            const float x = size.x * (0.10f + 0.80f * hash01(strike));
            drawLightning(draw, ImVec2(x, 0.0f), ImVec2(x + (hash01(strike + 5) - 0.5f) * 260.0f, size.y * (0.35f + 0.3f * hash01(strike + 9))),
                          strike, alpha);
        }
    }
}

// A dark panel with a gold double border, small gold diamonds at the corners
// and a Greek key band beside the title. Returns the area for content.
Box drawPoseidonPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    constexpr float kHeader = 26.0f;
    draw->AddRectFilled(box.min, box.max, withAlpha(kPoPanel, 0.84f), 3.0f);
    draw->AddRect(box.min, box.max, withAlpha(kPoGold, 0.85f), 3.0f, 0, 1.0f);
    draw->AddRect(box.min + ImVec2(3.0f, 3.0f), box.max - ImVec2(3.0f, 3.0f), withAlpha(kPoGold, 0.30f), 2.0f, 0, 1.0f);
    for (const ImVec2& corner : {box.min, ImVec2(box.max.x, box.min.y), box.max, ImVec2(box.min.x, box.max.y)}) {
        draw->AddQuadFilled(corner + ImVec2(0, -4), corner + ImVec2(4, 0), corner + ImVec2(0, 4), corner + ImVec2(-4, 0), withAlpha(kPoGold, 1.0f));
    }

    constexpr float kTitleSize = 12.0f;
    const ImVec2 titleExtent = textSize(state.displayFont, kTitleSize, title);
    const float titleY = std::floor(box.min.y + (kHeader - titleExtent.y) * 0.5f + 1.0f);
    draw->AddText(state.displayFont, kTitleSize, ImVec2(box.min.x + 14.0f, titleY), withAlpha(kPoGold, 1.0f), title.c_str());
    drawGreekKey(draw, box.min.x + 26.0f + titleExtent.x, box.max.x - 12.0f, box.min.y + 9.0f, 8.0f, withAlpha(kPoGold, 0.40f));
    draw->AddLine(ImVec2(box.min.x + 6.0f, box.min.y + kHeader + 0.5f), ImVec2(box.max.x - 6.0f, box.min.y + kHeader + 0.5f),
                  withAlpha(kPoGold, 0.25f));
    return Box{ImVec2(box.min.x + 14.0f, box.min.y + kHeader + 8.0f), ImVec2(box.max.x - 10.0f, box.max.y - 10.0f)};
}

// "LABEL   value": the label in small gold capitals, the value in marble white.
void drawPoseidonInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.displayFont, 10.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);   // line the small label up with the taller value
    ImGui::TextColored(kPoGold, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// A small trident lying along the radius at `angle`, prongs outwards.
void drawTrident(ImDrawList* draw, const ImVec2& center, float angle, float from, float to, ImU32 color) {
    const ImVec2 out(cosf(angle), sinf(angle)), side(-out.y, out.x);
    const float length = to - from;
    const ImVec2 head = center + out * (from + length * 0.60f), tip = center + out * to;
    const float spread = length * 0.24f;
    draw->AddLine(center + out * from, tip, color, 2.0f);                              // shaft and middle prong
    draw->AddLine(head - side * spread, head + side * spread, color, 2.0f);            // crossbar
    for (const float s : {-1.0f, 1.0f}) {                                              // outer prongs
        draw->AddLine(head + side * (spread * s), tip + side * (spread * s) - out * (length * 0.06f), color, 2.0f);
    }
}

// A Greek galley in silhouette, sailing right: a long hull with a curled
// stern and a ram at the bow, a mast with a square sail, oars, and two lines
// of waves. Centred on `center`; `size` is half its length.
void drawGalley(ImDrawList* draw, const ImVec2& center, float size, ImU32 color, ImU32 waveColor) {
    const auto at = [&](float x, float y) { return ImVec2(center.x + x * size, center.y + y * size); };
    const ImVec2 hull[] = {at(-0.98f, -0.42f), at(-0.86f, -0.40f), at(-0.80f, -0.12f), at(-0.62f, -0.04f), at(0.64f, -0.04f), at(0.78f, -0.16f),
                           at(0.84f, -0.12f),  at(0.80f, 0.02f),   at(1.00f, 0.10f),   at(0.78f, 0.12f),   at(0.60f, 0.22f), at(-0.60f, 0.22f),
                           at(-0.84f, 0.08f),  at(-0.92f, -0.18f)};
    draw->AddConcavePolyFilled(hull, IM_ARRAYSIZE(hull), color);
    draw->AddLine(at(0.0f, -0.04f), at(0.0f, -1.02f), color, std::max(size * 0.05f, 1.2f));         // mast
    draw->AddLine(at(-0.44f, -0.92f), at(0.44f, -0.92f), color, std::max(size * 0.05f, 1.2f));      // yard
    draw->AddQuadFilled(at(-0.40f, -0.90f), at(0.40f, -0.90f), at(0.34f, -0.22f), at(-0.34f, -0.22f), color);   // sail
    for (const float x : {-0.13f, 0.13f}) draw->AddLine(at(x, -0.88f), at(x * 0.85f, -0.24f), waveColor, 1.0f);   // its seams
    for (int i = 0; i < 6; ++i) {                                                                      // oars
        const float x = -0.48f + 0.19f * static_cast<float>(i);
        draw->AddLine(at(x, 0.18f), at(x - 0.14f, 0.42f), color, 1.0f);
    }
    for (int row = 0; row < 2; ++row) {                                                                // waves
        const float y = 0.40f + 0.18f * static_cast<float>(row);
        for (float x = -1.0f; x < 1.0f; x += 0.25f) {
            draw->PathArcTo(at(x + 0.125f + 0.06f * static_cast<float>(row), y), size * 0.125f, kPi * 1.1f, kPi * 1.9f, 8);
            draw->PathStroke(waveColor, ImDrawFlags_None, 1.0f);
        }
    }
}

// Black-figure silhouettes (as on Greek pottery) are built from rounded
// strokes: `stroke` draws a thick line through `points` with round joints.
void drawFigureStroke(ImDrawList* draw, std::initializer_list<ImVec2> points, float width, ImU32 color) {
    const ImVec2* p = points.begin();
    for (size_t i = 0; i + 1 < points.size(); ++i) draw->AddLine(p[i], p[i + 1], color, width);
    for (const ImVec2& joint : points) draw->AddCircleFilled(joint, width * 0.5f, color);
}

// A trident with three barbed prongs, centred on `center` and tilted
// `tilt` radians clockwise from upright. `size` is about half its length.
void drawTridentEmblem(ImDrawList* draw, const ImVec2& center, float size, float tilt, ImU32 color) {
    const float c = cosf(tilt), s = sinf(tilt);
    const auto at = [&](float x, float y) {   // (0, 0) is the shaft's foot; the trident points up (-y) before tilting
        y += 0.78f;                           // turn about its middle
        return ImVec2(center.x + (x * c - y * s) * size, center.y + (x * s + y * c) * size);
    };
    const float w = std::max(size * 0.11f, 1.5f);
    drawFigureStroke(draw, {at(0.0f, 0.0f), at(0.0f, -1.55f)}, w, color);                                // shaft and middle prong
    drawFigureStroke(draw, {at(-0.34f, -1.40f), at(-0.34f, -1.05f), at(0.34f, -1.05f), at(0.34f, -1.40f)}, w, color);   // the fork
    for (const float x : {-0.34f, 0.0f, 0.34f}) {                                                         // barbed tips
        const float top = x == 0.0f ? -1.55f : -1.40f;
        draw->AddTriangleFilled(at(x, top - 0.22f), at(x - 0.11f, top + 0.02f), at(x + 0.11f, top + 0.02f), color);
    }
    draw->AddCircleFilled(at(0.0f, -0.30f), w * 0.9f, color);                                            // a knot on the shaft
}

// A thunderbolt: the classic zig-zag, centred on `center` and tilted `tilt`
// radians clockwise. `size` is half its length. `color` should be opaque, so
// the two halves join without a seam.
void drawThunderbolt(ImDrawList* draw, const ImVec2& center, float size, float tilt, ImU32 color) {
    // The zig-zag already leans right (its tips are 0.46 apart across, 2 along),
    // so turn it only by what is left of `tilt`.
    const float turn = tilt - atanf(0.46f / 2.0f);
    const float c = cosf(turn), s = sinf(turn);
    const auto at = [&](float x, float y) { return ImVec2(center.x + (x * c - y * s) * size, center.y + (x * s + y * c) * size); };
    // Drawn as two convex halves that meet along the middle; ImGui's filler for
    // concave shapes mis-filled this one's sharp corners.
    const ImVec2 upper[] = {at(0.22f, -1.0f), at(-0.34f, 0.10f), at(-0.03f, 0.10f), at(0.05f, -0.16f)};
    const ImVec2 lower[] = {at(-0.03f, 0.10f), at(-0.24f, 1.0f), at(0.36f, -0.16f), at(0.05f, -0.16f)};
    draw->AddConvexPolyFilled(upper, 4, color);
    draw->AddConvexPolyFilled(lower, 4, color);
}

// A warrior with a round shield and a spear, riding a horse that rears up.
// `ground` is under the horse's hind hooves; `size` is about its height.
void drawHorseman(ImDrawList* draw, const ImVec2& ground, float size, ImU32 color) {
    const auto at = [&](float x, float y) { return ImVec2(ground.x + x * size, ground.y + y * size); };
    const float leg = std::max(size * 0.06f, 1.4f);
    drawFigureStroke(draw, {at(-0.34f, -0.42f), at(-0.40f, -0.20f), at(-0.30f, 0.0f)}, leg, color);      // hind legs
    drawFigureStroke(draw, {at(-0.27f, -0.42f), at(-0.20f, -0.20f), at(-0.12f, 0.0f)}, leg, color);
    drawFigureStroke(draw, {at(-0.32f, -0.47f), at(0.20f, -0.74f)}, size * 0.22f, color);                // body
    drawFigureStroke(draw, {at(0.18f, -0.76f), at(0.30f, -1.02f)}, size * 0.11f, color);                 // neck
    drawFigureStroke(draw, {at(0.29f, -1.03f), at(0.45f, -0.97f)}, size * 0.09f, color);                 // head
    draw->AddTriangleFilled(at(0.26f, -1.06f), at(0.30f, -1.16f), at(0.33f, -1.05f), color);             // ear
    drawFigureStroke(draw, {at(0.22f, -0.68f), at(0.42f, -0.72f), at(0.40f, -0.54f)}, leg, color);       // front legs, kicking
    drawFigureStroke(draw, {at(0.17f, -0.66f), at(0.34f, -0.58f), at(0.28f, -0.44f)}, leg, color);
    drawFigureStroke(draw, {at(-0.44f, -0.50f), at(-0.54f, -0.36f), at(-0.56f, -0.24f)}, leg * 0.8f, color);   // tail

    drawFigureStroke(draw, {at(-0.06f, -0.70f), at(-0.09f, -0.98f)}, size * 0.11f, color);              // rider
    draw->AddCircleFilled(at(-0.09f, -1.08f), size * 0.07f, color);
    draw->AddTriangleFilled(at(-0.16f, -1.10f), at(-0.09f, -1.24f), at(-0.02f, -1.12f), color);          // helmet crest
    drawFigureStroke(draw, {at(-0.05f, -0.70f), at(0.07f, -0.60f), at(0.02f, -0.44f)}, leg, color);      // his leg
    drawFigureStroke(draw, {at(-0.08f, -0.95f), at(0.06f, -1.02f)}, leg, color);                          // spear arm
    draw->AddLine(at(-0.38f, -1.16f), at(0.62f, -0.88f), color, std::max(size * 0.03f, 1.2f));          // spear...
    draw->AddTriangleFilled(at(0.72f, -0.85f), at(0.60f, -0.83f), at(0.62f, -0.92f), color);             // ...and its point
    draw->AddCircleFilled(at(-0.15f, -0.84f), size * 0.12f, color);                                      // round shield
}

// A round bronze shield: a gold rim with ticks, a sea-blue track filling with
// the load colour over 270 degrees, a trident at the value and the number
// carved in the middle, and a small gold scene in the gap at the bottom (a
// different one on each shield). The readouts sit under the shield.
void drawPoseidonGauge(ImDrawList* draw, const ImVec2& center, float radius, const GaugeReading& reading, const HudState& state, int index) {
    const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
    const float fraction = reading.hasValue ? percent / 100.0f : 0.0f;
    const ImVec4& color = poseidonLoadColor(percent, reading.warnAt, reading.critAt);
    constexpr float kStart = kPi * 0.75f, kSweep = kPi * 1.5f;

    draw->AddCircleFilled(center, radius, withAlpha(kPoPanel, 0.90f), 64);
    draw->AddCircle(center, radius, withAlpha(kPoGold, 0.90f), 64, 2.0f);
    draw->AddCircle(center, radius - 4.0f, withAlpha(kPoGold, 0.30f), 64, 1.0f);
    for (int i = 0; i <= 10; ++i) {
        const float angle = kStart + kSweep * static_cast<float>(i) / 10.0f;
        draw->AddLine(pointOnCircle(center, radius - 7.0f, angle), pointOnCircle(center, radius - (i % 5 == 0 ? 15.0f : 11.0f), angle),
                      withAlpha(kPoGold, 0.70f), i % 5 == 0 ? 2.0f : 1.0f);
    }

    const float arcRadius = radius * 0.70f;
    const float thickness = std::max(radius * 0.11f, 4.0f);
    draw->PathArcTo(center, arcRadius, kStart, kStart + kSweep);
    draw->PathStroke(withAlpha(kPoSea, 0.13f), thickness);
    if (fraction > 0.0f) drawGlowArc(draw, center, arcRadius, kStart, kStart + kSweep * fraction, color, thickness);
    if (reading.hasValue) {
        drawTrident(draw, center, kStart + kSweep * fraction, arcRadius - thickness * 0.5f - 4.0f, radius - 3.0f, withAlpha(kPoMarble, 1.0f));
    }

    const float numberSize = std::round(radius * 0.36f);
    if (reading.hasValue) {
        const std::string number = format("%.0f", percent);
        const float signSize = std::round(numberSize * 0.5f);
        const ImVec2 numberExtent = textSize(state.displayFont, numberSize, number);
        const ImVec2 signExtent = textSize(state.displayFont, signSize, "%");
        const ImVec2 pos(std::floor(center.x - (numberExtent.x + signExtent.x + 2.0f) * 0.5f), std::floor(center.y - numberExtent.y * 0.62f));
        draw->AddText(state.displayFont, numberSize, pos, withAlpha(kPoMarble, 1.0f), number.c_str());
        draw->AddText(state.displayFont, signSize, ImVec2(pos.x + numberExtent.x + 2.0f, pos.y + numberExtent.y - signExtent.y - 3.0f),
                      withAlpha(kPoGold, 1.0f), "%");
    } else {
        drawCenteredText(draw, state.displayFont, std::round(numberSize * 0.6f), ImVec2(center.x, center.y - numberSize * 0.2f),
                         withAlpha(kPoFoam, 1.0f), "N/A");
    }
    drawCenteredText(draw, state.displayFont, std::max(std::round(radius * 0.11f), 9.0f), ImVec2(center.x, center.y + radius * 0.24f),
                     withAlpha(kPoGold, 1.0f), reading.label);

    const ImU32 gold = withAlpha(kPoGold, 0.85f);
    const float ground = center.y + radius * 0.90f;
    constexpr float kTilt = kPi * 35.0f / 180.0f;   // the trident and the thunderbolt lean right by this much
    switch (index) {
        case 0:  drawGalley(draw, ImVec2(center.x, center.y + radius * 0.66f), radius * 0.30f, gold, withAlpha(kPoSea, 0.8f)); break;
        case 1:  drawTridentEmblem(draw, ImVec2(center.x, center.y + radius * 0.62f), radius * 0.255f, kTilt, gold); break;
        case 2:  drawThunderbolt(draw, ImVec2(center.x, center.y + radius * 0.62f), radius * 0.255f * 0.89f, kTilt, withAlpha(kPoGold, 1.0f)); break;   // as long as the trident
        default: drawHorseman(draw, ImVec2(center.x + radius * 0.03f, ground - radius * 0.03f), radius * 0.44f, gold); break;   // horse and rider centred; the spear reaches past
    }

    drawCenteredText(draw, state.textFont, 15.0f, ImVec2(center.x, center.y + radius + 14.0f), withAlpha(kPoMarble, 1.0f), reading.line1);
    drawCenteredText(draw, state.textFont, 13.0f, ImVec2(center.x, center.y + radius + 31.0f), withAlpha(kPoFoam, 1.0f), reading.line2);
}

void drawPoseidonGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    const float columnWidth = box.width() / 4.0f;
    const float radius = std::floor(std::min(columnWidth * 0.38f, (box.height() - 42.0f) * 0.5f));   // room underneath for the readouts
    const float centerY = std::floor(box.min.y + radius + 2.0f);
    for (int i = 0; i < 4; ++i) {
        drawPoseidonGauge(draw, ImVec2(std::floor(box.min.x + columnWidth * (static_cast<float>(i) + 0.5f)), centerY), radius, readings[i], state, i);
        if (i > 0) {   // between shields: a gold diamond
            const ImVec2 c(std::floor(box.min.x + columnWidth * static_cast<float>(i)), centerY);
            draw->AddQuadFilled(c + ImVec2(0, -6), c + ImVec2(6, 0), c + ImVec2(0, 6), c + ImVec2(-6, 0), withAlpha(kPoGold, 0.8f));
        }
    }
}

// The cores as a colonnade: temple columns on a stepped base, each filling
// with sea water to its load. The water's surface sways while animating.
void drawPoseidonCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawPoseidonPanelFrame(draw, box, format("COLONNADE \xC2\xB7 %zu THREADS", state.cores.size()), state), "##PoseidonCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kValueRow = 15.0f, kLabelRow = 14.0f, kStep = 6.0f;
    const float floorY = std::floor(start.y + std::max(room.y - kLabelRow - kStep, kValueRow + 30.0f));
    const float topY = start.y + kValueRow;
    panel->AddRectFilled(ImVec2(start.x, floorY), ImVec2(start.x + room.x, floorY + 3.0f), withAlpha(kPoGold, 0.55f));   // the steps
    panel->AddRectFilled(ImVec2(start.x + 6.0f, floorY + 3.0f), ImVec2(start.x + room.x - 6.0f, floorY + kStep), withAlpha(kPoGold, 0.25f));

    const int count = static_cast<int>(state.cores.size());
    const float slot = room.x / static_cast<float>(std::max(count, 1));
    const float width = std::clamp(std::floor(slot * 0.52f), 4.0f, 22.0f);
    for (int i = 0; i < count; ++i) {
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float x = std::floor(start.x + slot * (static_cast<float>(i) + 0.5f));
        const float capital = std::max(std::floor(width * 0.25f), 3.0f);
        const Box shaft{ImVec2(x - width * 0.5f, topY + capital + 1.0f), ImVec2(x + width * 0.5f, floorY - capital)};

        panel->AddRectFilled(shaft.min, shaft.max, withAlpha(kPoMarble, 0.10f));
        const float surface = std::floor(shaft.max.y - shaft.height() * value / 100.0f);
        const ImVec4& color = poseidonLoadColor(value, 0.50f, 0.80f);
        if (value > 0.5f) {
            panel->AddRectFilledMultiColor(ImVec2(shaft.min.x, surface), shaft.max, withAlpha(color, 0.85f), withAlpha(color, 0.85f),
                                           withAlpha(color, 0.35f), withAlpha(color, 0.35f));
            const float sway = sinf(state.time * 2.0f + static_cast<float>(i)) * 1.0f;
            panel->AddLine(ImVec2(shaft.min.x, surface + sway), ImVec2(shaft.max.x, surface - sway), withAlpha(kPoMarble, 0.8f), 1.0f);
        }
        if (width >= 10.0f) {   // fluting
            for (const float f : {-0.25f, 0.0f, 0.25f}) {
                const float fx = std::floor(x + width * f) + 0.5f;
                panel->AddLine(ImVec2(fx, shaft.min.y + 2.0f), ImVec2(fx, shaft.max.y - 2.0f), withAlpha(kPoPanel, 0.45f));
            }
        }
        const float overhang = std::max(std::floor(width * 0.2f), 2.0f);
        panel->AddRectFilled(ImVec2(shaft.min.x - overhang, topY), ImVec2(shaft.max.x + overhang, topY + capital), withAlpha(kPoGold, 0.9f));
        panel->AddRectFilled(ImVec2(shaft.min.x - overhang, floorY - capital), ImVec2(shaft.max.x + overhang, floorY), withAlpha(kPoGold, 0.9f));

        drawCenteredText(panel, state.displayFont, 8.0f, ImVec2(x, floorY + kStep + kLabelRow * 0.5f), withAlpha(kPoGold, 0.9f), format("%02d", i));
        if (slot >= 22.0f) {
            drawCenteredText(panel, state.textFont, 12.0f, ImVec2(x, start.y + kValueRow * 0.5f - 1.0f), withAlpha(kPoMarble, 0.9f),
                             format("%.0f", value));
        }
    }
    ImGui::Dummy(room);
    ImGui::EndChild();
}

// A small lightning bolt, centred on `center`.
void drawBoltIcon(ImDrawList* draw, const ImVec2& center, float size, ImU32 color) {
    draw->AddTriangleFilled(center + ImVec2(size * 0.25f, -size), center + ImVec2(-size * 0.55f, size * 0.15f), center + ImVec2(size * 0.15f, size * 0.15f),
                            color);
    draw->AddTriangleFilled(center + ImVec2(-size * 0.15f, -size * 0.15f), center + ImVec2(size * 0.55f, -size * 0.15f),
                            center + ImVec2(-size * 0.25f, size), color);
}

// The title in gold capitals with a soft glow, "ORACLE OF THE DEEP", a bolt
// that flickers for "LIVE", the clock, and a Greek key band underneath.
// Returns the buttons' rectangles.
HeaderButtons drawPoseidonHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 5.0f;

    const ImVec2 titleExtent = textSize(display, 22.0f, "VEEASTATS");
    drawGlowText(draw, display, 22.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), kPoGold, "VEEASTATS");
    const ImVec2 subtitleExtent = textSize(state.textFont, 14.0f, "Oracle of the Deep");
    draw->AddText(state.textFont, 14.0f, ImVec2(box.min.x + titleExtent.x + 14.0f, midY - subtitleExtent.y * 0.5f + 1.0f),
                  withAlpha(kPoFoam, 1.0f), "Oracle of the Deep");

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 18.0f;

    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(display, 16.0f, clock);
    right -= clockExtent.x;
    draw->AddText(display, 16.0f, ImVec2(right, midY - clockExtent.y * 0.5f), withAlpha(kPoMarble, 1.0f), clock.c_str());
    const ImVec2 dateExtent = textSize(display, 9.0f, date);
    right -= dateExtent.x + 12.0f;
    draw->AddText(display, 9.0f, ImVec2(right, midY - dateExtent.y * 0.5f), withAlpha(kPoFoam, 1.0f), date.c_str());

    // The bolt flickers now and then while animating.
    const float flicker = (state.animate && fmodf(state.time, 3.1f) < 0.15f) ? 0.35f : 1.0f;
    const ImVec2 liveExtent = textSize(display, 9.0f, "LIVE");
    right -= liveExtent.x + 22.0f;
    drawBoltIcon(draw, ImVec2(right - 6.0f, midY), 6.0f, withAlpha(kPoBolt, flicker));
    draw->AddText(display, 9.0f, ImVec2(right + 3.0f, midY - liveExtent.y * 0.5f), withAlpha(kPoBolt, 1.0f), "LIVE");

    drawGreekKey(draw, box.min.x, box.max.x, box.max.y - 9.0f, 7.0f, withAlpha(kPoGold, 0.55f));
    return buttons;
}

void applyPoseidonStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = style.PopupRounding = 3.0f;
    style.ChildRounding = style.FrameRounding = style.ScrollbarRounding = style.GrabRounding = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 8.0f;

    auto gold = [](float alpha) { return ImVec4(kPoGold.x, kPoGold.y, kPoGold.z, alpha); };
    auto sea = [](float alpha) { return ImVec4(kPoSea.x, kPoSea.y, kPoSea.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kPoMarble;
    colors[ImGuiCol_TextDisabled]         = kPoFoam;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.02f, 0.07f, 0.13f, 0.97f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.02f, 0.07f, 0.13f, 0.97f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = gold(0.55f);
    colors[ImGuiCol_FrameBg]              = sea(0.10f);
    colors[ImGuiCol_FrameBgHovered]       = sea(0.22f);
    colors[ImGuiCol_FrameBgActive]        = sea(0.32f);
    colors[ImGuiCol_Button]               = sea(0.14f);
    colors[ImGuiCol_ButtonHovered]        = sea(0.28f);
    colors[ImGuiCol_ButtonActive]         = sea(0.40f);
    colors[ImGuiCol_Header]               = sea(0.22f);
    colors[ImGuiCol_HeaderHovered]        = sea(0.32f);
    colors[ImGuiCol_HeaderActive]         = sea(0.42f);
    colors[ImGuiCol_CheckMark]            = kPoGold;
    colors[ImGuiCol_Separator]            = gold(0.30f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = gold(0.30f);
    colors[ImGuiCol_ScrollbarGrabHovered] = gold(0.45f);
    colors[ImGuiCol_ScrollbarGrabActive]  = gold(0.60f);
    colors[ImGuiCol_TextSelectedBg]       = sea(0.30f);
    colors[ImGuiCol_NavCursor]            = kPoGold;
}

// ---- Otaku ----------------------------------------------------------------------
// An anime mobile game's menus: a violet-to-pink sky with speed lines and
// twinkling sparkles, deep navy cards with glowing teal edges, slanted ribbon
// tabs, chunky outlined lettering and candy-coloured pill bars. The gauges are
// tilted character cards and the cores a list of stat bars.

const ImVec4 kOtNavy     (0.09f, 0.11f, 0.25f, 1.0f);   // cards
const ImVec4 kOtTeal     (0.32f, 0.92f, 0.88f, 1.0f);   // card edges; relaxed
const ImVec4 kOtPink     (1.00f, 0.40f, 0.68f, 1.0f);   // ribbons
const ImVec4 kOtViolet   (0.58f, 0.44f, 1.00f, 1.0f);   // chips
const ImVec4 kOtLemon    (1.00f, 0.84f, 0.30f, 1.0f);   // sparkles; busy
const ImVec4 kOtRed      (1.00f, 0.32f, 0.44f, 1.0f);   // critical
const ImVec4 kOtWhite    (1.00f, 1.00f, 1.00f, 1.0f);   // text
const ImVec4 kOtLilac    (0.80f, 0.78f, 1.00f, 1.0f);   // quiet text
const ImVec4 kOtInk      (0.11f, 0.06f, 0.24f, 1.0f);   // lettering outlines
const ImVec4 kOtSkyTop   (0.27f, 0.20f, 0.60f, 1.0f);   // background, top...
const ImVec4 kOtSkyBottom(0.60f, 0.24f, 0.52f, 1.0f);   // ...and bottom

// Teal when relaxed, lemon when busy, red when critical.
const ImVec4& otakuLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kOtRed : (fraction > warnAt) ? kOtLemon : kOtTeal;
}

// Text with a thick ink outline, like a mobile game's lettering.
void drawInkText(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, ImU32 color, const std::string& text, float outline = 1.5f) {
    for (int i = 0; i < 8; ++i) {
        const float angle = kPi * 0.25f * static_cast<float>(i);
        draw->AddText(font, size, pos + ImVec2(cosf(angle) * outline, sinf(angle) * outline), withAlpha(kOtInk, 1.0f), text.c_str());
    }
    draw->AddText(font, size, pos, color, text.c_str());
}

// A four-pointed sparkle.
void drawSparkle(ImDrawList* draw, const ImVec2& c, float size, ImU32 color) {
    const float waist = size * 0.26f;
    draw->AddQuadFilled(c + ImVec2(0, -size), c + ImVec2(waist, 0), c + ImVec2(0, size), c + ImVec2(-waist, 0), color);
    draw->AddQuadFilled(c + ImVec2(-size, 0), c + ImVec2(0, -waist), c + ImVec2(size, 0), c + ImVec2(0, waist), color);
}

// A pill-shaped bar: a dim track, the filled part in `color` with a glossy
// highlight along its top.
void drawPillBar(ImDrawList* draw, const Box& box, float percent, const ImVec4& color) {
    const float rounding = box.height() * 0.5f;
    draw->AddRectFilled(box.min, box.max, withAlpha(kOtWhite, 0.10f), rounding);
    const float fill = std::floor(box.width() * std::clamp(percent, 0.0f, 100.0f) / 100.0f);
    if (fill >= 2.0f) {
        const ImVec2 end(box.min.x + std::max(fill, box.height()), box.max.y);
        draw->AddRectFilled(box.min, end, withAlpha(color, 1.0f), rounding);
        draw->AddRectFilled(ImVec2(box.min.x + rounding * 0.6f, box.min.y + 1.0f), ImVec2(end.x - rounding * 0.6f, box.min.y + box.height() * 0.38f),
                            withAlpha(kOtWhite, 0.40f), rounding * 0.5f);
    }
    draw->AddRect(box.min, box.max, withAlpha(kOtInk, 0.9f), rounding, 0, 1.0f);
}

// A slanted ribbon tab with outlined lettering. Returns its width.
float drawRibbon(ImDrawList* draw, ImFont* font, float size, const ImVec2& topLeft, float height, const ImVec4& color, const std::string& text) {
    constexpr float kSlant = 8.0f;
    const ImVec2 extent = textSize(font, size, text);
    const float width = std::ceil(extent.x) + 26.0f;
    const ImVec2 a(topLeft.x + kSlant, topLeft.y), b(topLeft.x + width + kSlant, topLeft.y), c(topLeft.x + width, topLeft.y + height),
        d(topLeft.x, topLeft.y + height);
    draw->AddQuadFilled(a, b, c, d, withAlpha(color, 1.0f));
    draw->AddQuadFilled(a, b, ImVec2(b.x - kSlant * 0.4f, a.y + height * 0.4f), ImVec2(a.x - kSlant * 0.4f, a.y + height * 0.4f),
                        withAlpha(kOtWhite, 0.22f));
    draw->AddQuad(a, b, c, d, withAlpha(kOtInk, 0.9f), 1.5f);
    drawInkText(draw, font, size, ImVec2(std::floor(topLeft.x + 13.0f + kSlant * 0.5f), std::floor(topLeft.y + (height - extent.y) * 0.5f)),
                withAlpha(kOtWhite, 1.0f), text, 1.0f);
    return width + kSlant;
}

// A violet-to-pink sky with faint diagonal speed lines and sparkles that
// twinkle while animating.
void drawOtakuBackground(const ImVec2& size, const HudState& state) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, withAlpha(kOtSkyTop, 1.0f), withAlpha(mix(kOtSkyTop, kOtSkyBottom, 0.35f), 1.0f),
                                  withAlpha(kOtSkyBottom, 1.0f), withAlpha(mix(kOtSkyTop, kOtSkyBottom, 0.65f), 1.0f));
    for (int i = 0; i < 28; ++i) {
        const float x = std::fmod(hash01(i) * (size.x + 400.0f) + state.time * 30.0f, size.x + 400.0f) - 200.0f;
        const float length = 80.0f + hash01(i + 50) * 220.0f;
        draw->AddLine(ImVec2(x, size.y), ImVec2(x + length * 0.5f, size.y - length), withAlpha(kOtWhite, 0.03f + 0.04f * hash01(i + 90)),
                      1.0f + std::floor(hash01(i + 70) * 3.0f));
    }
    for (int i = 0; i < 26; ++i) {
        const ImVec2 at(hash01(i + 200) * size.x, hash01(i + 300) * size.y);
        const float twinkle = state.animate ? 0.5f + 0.5f * sinf(state.time * 2.2f + static_cast<float>(i) * 1.7f) : 0.7f;
        drawSparkle(draw, at, 2.0f + 4.0f * hash01(i + 400) * (0.6f + 0.4f * twinkle), withAlpha(i % 3 == 0 ? kOtLemon : kOtWhite, 0.15f + 0.35f * twinkle));
    }
}

// A rounded navy card with a glowing teal edge, and a slanted pink ribbon tab
// carrying the title across its top edge. Returns the area for content.
Box drawOtakuPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    constexpr float kRounding = 12.0f, kTabHeight = 20.0f;
    draw->AddRect(box.min - ImVec2(2, 2), box.max + ImVec2(2, 2), withAlpha(kOtTeal, 0.14f), kRounding + 2.0f, 0, 4.0f);
    draw->AddRectFilled(box.min, box.max, withAlpha(kOtNavy, 0.94f), kRounding);
    draw->AddRect(box.min, box.max, withAlpha(kOtTeal, 0.95f), kRounding, 0, 2.0f);
    draw->AddRect(box.min + ImVec2(4, 4), box.max - ImVec2(4, 4), withAlpha(kOtTeal, 0.16f), kRounding - 4.0f, 0, 1.0f);

    const float ribbon = drawRibbon(draw, state.displayFont, 11.0f, ImVec2(box.min.x + 16.0f, box.min.y - 7.0f), kTabHeight, kOtPink, title);
    drawSparkle(draw, ImVec2(box.min.x + 16.0f + ribbon + 10.0f, box.min.y + 3.0f), 5.0f, withAlpha(kOtLemon, 1.0f));
    return Box{ImVec2(box.min.x + 16.0f, box.min.y + kTabHeight + 2.0f), ImVec2(box.max.x - 12.0f, box.max.y - 10.0f)};
}

// "LABEL   value": the label in teal, the value in white.
void drawOtakuInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.textFont, 12.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);   // line the small label up with the taller value
    ImGui::TextColored(kOtTeal, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// One gauge as a tilted character card: a navy parallelogram with an edge in
// the card's own colour, a name ribbon, the number in big outlined lettering,
// a pill bar and the two readouts.
void drawOtakuCard(ImDrawList* draw, const Box& box, const GaugeReading& reading, const ImVec4& identity, const HudState& state, int index) {
    constexpr float kSlant = 16.0f;
    const auto edgeX = [&](float y, bool right) {   // the slanted sides: x at height y
        const float shift = kSlant * (1.0f - (y - box.min.y) / box.height());
        return right ? box.max.x - kSlant + shift : box.min.x + shift;
    };
    const ImVec2 corners[] = {ImVec2(edgeX(box.min.y, false), box.min.y), ImVec2(edgeX(box.min.y, true), box.min.y),
                              ImVec2(edgeX(box.max.y, true), box.max.y), ImVec2(edgeX(box.max.y, false), box.max.y)};
    draw->AddQuad(corners[0], corners[1], corners[2], corners[3], withAlpha(identity, 0.16f), 6.0f);
    draw->AddQuadFilled(corners[0], corners[1], corners[2], corners[3], withAlpha(kOtNavy, 0.95f));
    const float bandBottom = box.min.y + box.height() * 0.30f;
    draw->AddQuadFilled(corners[0], corners[1], ImVec2(edgeX(bandBottom, true), bandBottom), ImVec2(edgeX(bandBottom, false), bandBottom),
                        withAlpha(identity, 0.20f));
    draw->AddQuad(corners[0], corners[1], corners[2], corners[3], withAlpha(identity, 0.95f), 2.0f);

    const float left = box.min.x + kSlant + 10.0f, right = box.max.x - kSlant - 6.0f;
    drawRibbon(draw, state.displayFont, 11.0f, ImVec2(left - 4.0f, box.min.y + 10.0f), 20.0f, index == 3 ? kOtPink : identity,
               reading.label);
    const float twinkle = state.animate ? 0.6f + 0.4f * sinf(state.time * 3.0f + static_cast<float>(index) * 1.3f) : 1.0f;
    drawSparkle(draw, ImVec2(box.max.x - 14.0f, box.min.y + 18.0f), 6.0f * twinkle, withAlpha(kOtLemon, 1.0f));
    drawSparkle(draw, ImVec2(box.max.x - 26.0f, box.min.y + 30.0f), 3.0f, withAlpha(kOtWhite, 0.8f));

    // The number, big, with its "%" in the card's colour.
    const float numberSize = std::round(std::clamp(box.height() * 0.24f, 30.0f, 52.0f));
    const float numberTop = std::floor(box.min.y + box.height() * 0.30f);
    const float centerX = std::floor((left + right) * 0.5f);
    if (reading.hasValue) {
        const std::string number = format("%.0f", std::clamp(reading.percent, 0.0f, 100.0f));
        const ImVec2 numberExtent = textSize(state.displayFont, numberSize, number);
        const ImVec2 signExtent = textSize(state.displayFont, numberSize * 0.5f, "%");
        const float x = std::floor(centerX - (numberExtent.x + signExtent.x + 3.0f) * 0.5f);
        drawInkText(draw, state.displayFont, numberSize, ImVec2(x, numberTop), withAlpha(kOtWhite, 1.0f), number, 2.0f);
        drawInkText(draw, state.displayFont, numberSize * 0.5f, ImVec2(x + numberExtent.x + 3.0f, numberTop + numberExtent.y - signExtent.y - 4.0f),
                    withAlpha(identity, 1.0f), "%", 1.5f);
    } else {
        const ImVec2 extent = textSize(state.displayFont, numberSize * 0.6f, "N/A");
        drawInkText(draw, state.displayFont, numberSize * 0.6f, ImVec2(centerX - extent.x * 0.5f, numberTop + numberSize * 0.2f),
                    withAlpha(kOtLilac, 1.0f), "N/A", 1.5f);
    }

    const float barTop = std::floor(numberTop + numberSize * 1.30f);
    drawPillBar(draw, Box{ImVec2(left, barTop), ImVec2(right, barTop + 12.0f)}, reading.hasValue ? reading.percent : 0.0f,
                otakuLoadColor(reading.percent, reading.warnAt, reading.critAt));
    drawCenteredText(draw, state.textFont, 14.0f, ImVec2(centerX, barTop + 28.0f), withAlpha(kOtWhite, 1.0f), reading.line1);
    drawCenteredText(draw, state.textFont, 12.0f, ImVec2(centerX, barTop + 46.0f), withAlpha(kOtLilac, 1.0f), reading.line2);
}

void drawOtakuGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    static const ImVec4 kIdentities[] = {kOtTeal, kOtViolet, kOtPink, kOtLemon};
    constexpr float kGap = 10.0f;
    const float width = std::floor((box.width() - kGap * 3.0f) / 4.0f);
    for (int i = 0; i < 4; ++i) {
        const float x = box.min.x + static_cast<float>(i) * (width + kGap);
        drawOtakuCard(draw, Box{ImVec2(x, box.min.y + 4.0f), ImVec2(x + width, box.max.y - 2.0f)}, readings[i], kIdentities[i], state, i);
    }
}

// The cores as a game's stat list: a numbered chip, a pill bar and the
// percentage, in as many columns as it takes to fit.
void drawOtakuCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawOtakuPanelFrame(draw, box, format("CORES \xC2\xB7 %zu THREADS", state.cores.size()), state), "##OtakuCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kRowHeight = 19.0f, kColumnGap = 16.0f;
    const int count = static_cast<int>(state.cores.size());
    const int rowsThatFit = std::max(static_cast<int>(room.y / kRowHeight), 1);
    const int columns = std::max((count + rowsThatFit - 1) / rowsThatFit, 1);
    const int rows = (count + columns - 1) / columns;
    const float columnWidth = (room.x - kColumnGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (int i = 0; i < count; ++i) {
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float x = std::floor(start.x + static_cast<float>(i / rows) * (columnWidth + kColumnGap));
        const float midY = std::floor(start.y + (static_cast<float>(i % rows) + 0.5f) * kRowHeight);

        const Box chip{ImVec2(x, midY - 7.0f), ImVec2(x + 30.0f, midY + 7.0f)};
        panel->AddRectFilled(chip.min, chip.max, withAlpha(kOtViolet, 0.9f), 7.0f);
        drawCenteredText(panel, state.textFont, 11.0f, ImVec2((chip.min.x + chip.max.x) * 0.5f, midY), withAlpha(kOtWhite, 1.0f), format("C%02d", state.coreId(i)));
        const float barRight = x + columnWidth - 40.0f;
        drawPillBar(panel, Box{ImVec2(chip.max.x + 6.0f, midY - 4.0f), ImVec2(barRight, midY + 5.0f)}, value, otakuLoadColor(value, 0.50f, 0.80f));
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.textFont, 13.0f, text);
        panel->AddText(state.textFont, 13.0f, ImVec2(x + columnWidth - extent.x, midY - extent.y * 0.5f), withAlpha(kOtWhite, 1.0f), text.c_str());
    }
    ImGui::Dummy(room);
    ImGui::EndChild();
}

// A heart, for "LIVE".
void drawHeart(ImDrawList* draw, const ImVec2& c, float size, ImU32 color) {
    draw->AddCircleFilled(c + ImVec2(-size * 0.5f, -size * 0.2f), size * 0.55f, color);
    draw->AddCircleFilled(c + ImVec2(size * 0.5f, -size * 0.2f), size * 0.55f, color);
    draw->AddTriangleFilled(c + ImVec2(-size * 1.03f, 0.0f), c + ImVec2(size * 1.03f, 0.0f), c + ImVec2(0.0f, size * 1.1f), color);
}

// The title in big outlined lettering with a pink shadow, a "SYSTEM STATUS"
// ribbon, and a pill-shaped counter on the right holding a beating heart for
// "LIVE", the date and the clock. Returns the buttons' rectangles.
HeaderButtons drawOtakuHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 3.0f;

    const ImVec2 titleExtent = textSize(display, 24.0f, "VeeaStats");
    const ImVec2 titlePos(box.min.x + 2.0f, std::floor(midY - titleExtent.y * 0.5f));
    draw->AddText(display, 24.0f, titlePos + ImVec2(3.0f, 3.0f), withAlpha(kOtPink, 1.0f), "VeeaStats");
    drawInkText(draw, display, 24.0f, titlePos, withAlpha(kOtWhite, 1.0f), "VeeaStats", 2.0f);
    drawRibbon(draw, state.textFont, 11.0f, ImVec2(box.min.x + titleExtent.x + 18.0f, midY - 9.0f), 18.0f, kOtViolet, "SYSTEM STATUS");

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(display, 14.0f, clock), dateExtent = textSize(state.textFont, 12.0f, date);
    const float pillRight = buttons.storage.min.x - 12.0f;
    const float pillLeft = pillRight - (clockExtent.x + dateExtent.x + 58.0f);
    const Box pill{ImVec2(pillLeft, midY - 12.0f), ImVec2(pillRight, midY + 12.0f)};
    draw->AddRectFilled(pill.min, pill.max, withAlpha(kOtNavy, 0.92f), 12.0f);
    draw->AddRect(pill.min, pill.max, withAlpha(kOtTeal, 0.95f), 12.0f, 0, 2.0f);
    const float beat = state.animate ? 1.0f + 0.15f * std::max(sinf(state.time * 6.0f), 0.0f) : 1.0f;
    drawHeart(draw, ImVec2(pill.min.x + 16.0f, midY - 1.0f), 5.5f * beat, withAlpha(kOtPink, 1.0f));
    draw->AddText(state.textFont, 12.0f, ImVec2(pill.min.x + 30.0f, midY - dateExtent.y * 0.5f), withAlpha(kOtLilac, 1.0f), date.c_str());
    drawInkText(draw, display, 14.0f, ImVec2(pill.max.x - 12.0f - clockExtent.x, midY - clockExtent.y * 0.5f), withAlpha(kOtWhite, 1.0f), clock, 1.0f);
    return buttons;
}

void applyOtakuStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.WindowRounding = style.PopupRounding = 12.0f;
    style.ChildRounding = style.FrameRounding = style.ScrollbarRounding = style.GrabRounding = 8.0f;
    style.WindowBorderSize = style.PopupBorderSize = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 8.0f;

    auto teal = [](float alpha) { return ImVec4(kOtTeal.x, kOtTeal.y, kOtTeal.z, alpha); };
    auto violet = [](float alpha) { return ImVec4(kOtViolet.x, kOtViolet.y, kOtViolet.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kOtWhite;
    colors[ImGuiCol_TextDisabled]         = kOtLilac;
    colors[ImGuiCol_WindowBg]             = ImVec4(kOtNavy.x, kOtNavy.y, kOtNavy.z, 0.98f);
    colors[ImGuiCol_PopupBg]              = ImVec4(kOtNavy.x, kOtNavy.y, kOtNavy.z, 0.98f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = teal(0.85f);
    colors[ImGuiCol_FrameBg]              = violet(0.18f);
    colors[ImGuiCol_FrameBgHovered]       = violet(0.32f);
    colors[ImGuiCol_FrameBgActive]        = violet(0.45f);
    colors[ImGuiCol_Button]               = violet(0.30f);
    colors[ImGuiCol_ButtonHovered]        = violet(0.45f);
    colors[ImGuiCol_ButtonActive]         = violet(0.60f);
    colors[ImGuiCol_Header]               = ImVec4(kOtPink.x, kOtPink.y, kOtPink.z, 0.35f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(kOtPink.x, kOtPink.y, kOtPink.z, 0.50f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(kOtPink.x, kOtPink.y, kOtPink.z, 0.65f);
    colors[ImGuiCol_CheckMark]            = kOtPink;
    colors[ImGuiCol_Separator]            = teal(0.30f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = teal(0.35f);
    colors[ImGuiCol_ScrollbarGrabHovered] = teal(0.50f);
    colors[ImGuiCol_ScrollbarGrabActive]  = teal(0.65f);
    colors[ImGuiCol_TextSelectedBg]       = violet(0.40f);
    colors[ImGuiCol_NavCursor]            = kOtPink;
}

// ---- Cyber-Punk -------------------------------------------------------------------
// A neon city's heads-up display: thin red line work on near-black with
// scanlines, panels with cut corners, hexagon gauges joined by circuit traces,
// slanted segment bars, cyan readouts, and a yellow title that glitches now
// and then.

const ImVec4 kCpRed    (1.00f, 0.33f, 0.34f, 1.0f);    // line work, labels
const ImVec4 kCpDimRed (0.55f, 0.15f, 0.17f, 1.0f);    // tracks, traces
const ImVec4 kCpCyan   (0.37f, 0.96f, 1.00f, 1.0f);    // values; relaxed
const ImVec4 kCpYellow (0.99f, 0.93f, 0.04f, 1.0f);    // title; busy
const ImVec4 kCpHot    (1.00f, 0.16f, 0.45f, 1.0f);    // critical
const ImVec4 kCpText   (0.95f, 0.88f, 0.86f, 1.0f);    // ordinary text
const ImVec4 kCpPanel  (0.07f, 0.025f, 0.035f, 1.0f);  // panel fill
const ImVec4 kCpBlack  (0.025f, 0.01f, 0.015f, 1.0f);  // background

// Cyan when relaxed, yellow when busy, hot pink when critical.
const ImVec4& cyberpunkLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kCpHot : (fraction > warnAt) ? kCpYellow : kCpCyan;
}

// The six corners of a box with its top-right and bottom-left corners cut off.
std::array<ImVec2, 6> cutCorners(const Box& box, float cut) {
    return {{box.min, ImVec2(box.max.x - cut, box.min.y), ImVec2(box.max.x, box.min.y + cut), box.max, ImVec2(box.min.x + cut, box.max.y),
             ImVec2(box.min.x, box.max.y - cut)}};
}

// A row of slanted blocks lit up to `percent`, like a health bar.
void drawSlantedSegments(ImDrawList* draw, const Box& box, float percent, const ImVec4& color) {
    constexpr float kGap = 2.0f;
    const float slant = box.height() * 0.5f;
    const int count = std::clamp(static_cast<int>(box.width() / 7.0f), 6, 40);
    const float step = (box.width() - slant) / static_cast<float>(count);
    const float lit = std::clamp(percent, 0.0f, 100.0f) / 100.0f * static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const float x = std::floor(box.min.x + step * static_cast<float>(i));
        const float w = std::max(step - kGap, 1.0f);
        const ImVec2 a(x + slant, box.min.y), b(x + slant + w, box.min.y), c(x + w, box.max.y), d(x, box.max.y);
        const float amount = std::clamp(lit - static_cast<float>(i), 0.0f, 1.0f);
        draw->AddQuadFilled(a, b, c, d, amount > 0.0f ? withAlpha(color, 0.35f + 0.65f * amount) : withAlpha(kCpDimRed, 0.45f));
    }
}

// Near-black with a dot grid, scanlines, hatched corners and, while
// animating, a short glitch every few seconds.
void drawCyberpunkBackground(const ImVec2& size, const HudState& state) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 edge = withAlpha(kCpBlack, 1.0f), middle = withAlpha(ImVec4(0.08f, 0.02f, 0.03f, 1.0f), 1.0f);
    draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, middle, edge, middle, edge);

    constexpr float kDotStep = 24.0f;
    for (float y = kDotStep; y < size.y; y += kDotStep) {
        for (float x = kDotStep; x < size.x; x += kDotStep) draw->AddRectFilled(ImVec2(x, y), ImVec2(x + 1.0f, y + 1.0f), withAlpha(kCpRed, 0.16f));
    }
    for (float y = 0.0f; y < size.y; y += 3.0f) draw->AddLine(ImVec2(0.0f, y), ImVec2(size.x, y), IM_COL32(0, 0, 0, 40));

    // Hazard hatching in the bottom-left and top-right corners.
    for (int i = 0; i < 9; ++i) {
        const float o = static_cast<float>(i) * 9.0f;
        draw->AddLine(ImVec2(o, size.y), ImVec2(o + 60.0f, size.y - 60.0f), withAlpha(kCpRed, 0.10f), 3.0f);
        draw->AddLine(ImVec2(size.x - o, 0.0f), ImVec2(size.x - o - 60.0f, 60.0f), withAlpha(kCpRed, 0.10f), 3.0f);
    }

    if (state.animate) {
        constexpr float kEvery = 5.0f;
        if (fmodf(state.time, kEvery) < 0.14f) {
            const int glitch = static_cast<int>(state.time / kEvery);
            for (int i = 0; i < 3; ++i) {
                const float y = hash01(glitch * 13 + i) * size.y;
                const float height = 2.0f + hash01(glitch * 17 + i) * 10.0f;
                draw->AddRectFilled(ImVec2(0.0f, y), ImVec2(size.x, y + height), withAlpha(i == 1 ? kCpCyan : kCpRed, 0.10f));
            }
        }
    }
}

// A dark panel with cut corners and a red outline; the title in red after a
// small square, a rule running off to the right ending in a short code, and
// a bright red tick along the top of the left edge. Returns the area for content.
Box drawCyberpunkPanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    constexpr float kCut = 14.0f, kHeader = 24.0f;
    const std::array<ImVec2, 6> outline = cutCorners(box, kCut);
    draw->AddConvexPolyFilled(outline.data(), static_cast<int>(outline.size()), withAlpha(kCpPanel, 0.88f));
    draw->AddPolyline(outline.data(), static_cast<int>(outline.size()), withAlpha(kCpRed, 0.55f), ImDrawFlags_Closed, 1.0f);
    draw->AddRectFilled(ImVec2(box.min.x - 1.0f, box.min.y + 4.0f), ImVec2(box.min.x + 2.0f, box.min.y + 34.0f), withAlpha(kCpRed, 1.0f));
    draw->AddLine(outline[4] + ImVec2(-1, 0), outline[5] + ImVec2(-1, 0), withAlpha(kCpRed, 1.0f), 2.0f);   // the cut, bright

    constexpr float kTitleSize = 11.0f;
    const ImVec2 titleExtent = textSize(state.displayFont, kTitleSize, title);
    const float midY = std::floor(box.min.y + kHeader * 0.5f + 1.0f);
    draw->AddRectFilled(ImVec2(box.min.x + 12.0f, midY - 3.0f), ImVec2(box.min.x + 18.0f, midY + 3.0f), withAlpha(kCpRed, 1.0f));
    draw->AddText(state.displayFont, kTitleSize, ImVec2(box.min.x + 24.0f, std::floor(midY - titleExtent.y * 0.5f)), withAlpha(kCpRed, 1.0f),
                  title.c_str());
    const std::string code = format("%04X", static_cast<unsigned>(hash01(static_cast<int>(title.size()) * 97 + title[0]) * 65535.0f));
    const ImVec2 codeExtent = textSize(state.textFont, 12.0f, code);
    const float codeX = box.max.x - kCut - 10.0f - codeExtent.x;
    draw->AddText(state.textFont, 12.0f, ImVec2(codeX, std::floor(midY - codeExtent.y * 0.5f)), withAlpha(kCpDimRed, 1.0f), code.c_str());
    draw->AddLine(ImVec2(box.min.x + 32.0f + titleExtent.x, midY + 0.5f), ImVec2(codeX - 8.0f, midY + 0.5f), withAlpha(kCpRed, 0.30f));
    return Box{ImVec2(box.min.x + 14.0f, box.min.y + kHeader + 6.0f), ImVec2(box.max.x - 12.0f, box.max.y - 10.0f)};
}

// "LABEL   value": the label small and red, the value in pale text.
void drawCyberpunkInfoRow(const HudState& state, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(state.displayFont, 10.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);   // line the small label up with the taller value
    ImGui::TextColored(kCpRed, "%s", label);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// The corners of a pointy-topped hexagon, clockwise from the top.
std::array<ImVec2, 6> hexagon(const ImVec2& center, float radius) {
    std::array<ImVec2, 6> points;
    for (int i = 0; i < 6; ++i) points[static_cast<size_t>(i)] = pointOnCircle(center, radius, -kPi * 0.5f + kPi / 3.0f * static_cast<float>(i));
    return points;
}

// A hexagon gauge: its outline lights up clockwise from the top to the value,
// round a dark inner hexagon holding the label and number. The readouts sit
// underneath.
void drawCyberpunkGauge(ImDrawList* draw, const ImVec2& center, float radius, const GaugeReading& reading, const HudState& state) {
    const float percent = std::clamp(reading.percent, 0.0f, 100.0f);
    const float fraction = reading.hasValue ? percent / 100.0f : 0.0f;
    const ImVec4& color = cyberpunkLoadColor(percent, reading.warnAt, reading.critAt);

    const std::array<ImVec2, 6> outer = hexagon(center, radius), inner = hexagon(center, radius * 0.80f);
    draw->AddConvexPolyFilled(inner.data(), 6, withAlpha(kCpPanel, 0.95f));
    draw->AddPolyline(inner.data(), 6, withAlpha(kCpRed, 0.40f), ImDrawFlags_Closed, 1.0f);
    draw->AddPolyline(outer.data(), 6, withAlpha(kCpDimRed, 0.70f), ImDrawFlags_Closed, 4.0f);

    // The lit part of the outline: whole sides, then part of the next one.
    const float sides = fraction * 6.0f;
    std::vector<ImVec2> lit = {outer[0]};
    for (int i = 0; i < 6 && static_cast<float>(i) < sides; ++i) {
        const ImVec2& from = outer[static_cast<size_t>(i)];
        const ImVec2& to = outer[static_cast<size_t>((i + 1) % 6)];
        lit.push_back(from + (to - from) * std::min(sides - static_cast<float>(i), 1.0f));
    }
    if (lit.size() > 1) {
        for (const auto& pass : kGlowPasses) {
            draw->AddPolyline(lit.data(), static_cast<int>(lit.size()), withAlpha(color, pass[1]), ImDrawFlags_None, 4.0f + pass[0]);
        }
        draw->AddPolyline(lit.data(), static_cast<int>(lit.size()), withAlpha(color, 1.0f), ImDrawFlags_None, 4.0f);
    }
    for (const ImVec2& corner : outer) {   // a small notch at each corner
        draw->AddRectFilled(corner - ImVec2(2, 2), corner + ImVec2(2, 2), withAlpha(kCpRed, 0.9f));
    }

    drawCenteredText(draw, state.displayFont, std::max(std::round(radius * 0.13f), 9.0f), ImVec2(center.x, center.y - radius * 0.36f),
                     withAlpha(kCpRed, 1.0f), reading.label);
    const float numberSize = std::round(radius * 0.42f);
    if (reading.hasValue) {
        const std::string number = format("%.0f", percent);
        const float signSize = std::round(numberSize * 0.45f);
        const ImVec2 numberExtent = textSize(state.displayFont, numberSize, number);
        const ImVec2 signExtent = textSize(state.displayFont, signSize, "%");
        const ImVec2 pos(std::floor(center.x - (numberExtent.x + signExtent.x + 2.0f) * 0.5f), std::floor(center.y - numberExtent.y * 0.38f));
        draw->AddText(state.displayFont, numberSize, pos, withAlpha(color, 1.0f), number.c_str());
        draw->AddText(state.displayFont, signSize, ImVec2(pos.x + numberExtent.x + 2.0f, pos.y + numberExtent.y - signExtent.y - 3.0f),
                      withAlpha(kCpRed, 1.0f), "%");
    } else {
        drawCenteredText(draw, state.displayFont, std::round(numberSize * 0.6f), ImVec2(center.x, center.y + numberSize * 0.1f),
                         withAlpha(kCpDimRed, 1.0f), "N/A");
    }

    drawCenteredText(draw, state.textFont, 16.0f, ImVec2(center.x, center.y + radius + 14.0f), withAlpha(kCpText, 1.0f), reading.line1);
    drawCenteredText(draw, state.textFont, 14.0f, ImVec2(center.x, center.y + radius + 31.0f), withAlpha(kCpRed, 0.85f), reading.line2);
}

void drawCyberpunkGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    const float columnWidth = box.width() / 4.0f;
    const float radius = std::floor(std::min(columnWidth * 0.36f, (box.height() - 46.0f) * 0.5f));
    const float centerY = std::floor(box.min.y + radius + 4.0f);
    const float halfWidth = radius * 0.866f;   // a pointy-topped hexagon's half width
    for (int i = 0; i < 4; ++i) {
        const float centerX = std::floor(box.min.x + columnWidth * (static_cast<float>(i) + 0.5f));
        if (i < 3) {   // a circuit trace to the next gauge, with a little chip half way
            const float from = centerX + halfWidth + 4.0f, to = centerX + columnWidth - halfWidth - 4.0f;
            const float y = centerY + 0.5f;
            draw->AddLine(ImVec2(from, y), ImVec2(to, y), withAlpha(kCpDimRed, 0.9f), 1.0f);
            draw->AddLine(ImVec2(from + 10.0f, y + 8.0f), ImVec2(to - 10.0f, y + 8.0f), withAlpha(kCpDimRed, 0.5f), 1.0f);
            const float mid = std::floor((from + to) * 0.5f);
            draw->AddRect(ImVec2(mid - 5.0f, y - 4.0f), ImVec2(mid + 5.0f, y + 4.0f), withAlpha(kCpRed, 0.8f));
        }
        drawCyberpunkGauge(draw, ImVec2(centerX, centerY), radius, readings[i], state);
    }
}

// The cores as rows of slanted segment bars, in as many columns as it takes.
void drawCyberpunkCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawCyberpunkPanelFrame(draw, box, format("CORE STATUS // %zu THREADS", state.cores.size()), state), "##CyberpunkCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kRowHeight = 18.0f, kColumnGap = 18.0f;
    const int count = static_cast<int>(state.cores.size());
    const int rowsThatFit = std::max(static_cast<int>(room.y / kRowHeight), 1);
    const int columns = std::max((count + rowsThatFit - 1) / rowsThatFit, 1);
    const int rows = (count + columns - 1) / columns;
    const float columnWidth = (room.x - kColumnGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (int i = 0; i < count; ++i) {
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float x = std::floor(start.x + static_cast<float>(i / rows) * (columnWidth + kColumnGap));
        const float midY = std::floor(start.y + (static_cast<float>(i % rows) + 0.5f) * kRowHeight);
        const std::string label = format("C%02d", state.coreId(i));
        const ImVec2 labelExtent = textSize(state.displayFont, 9.0f, label);
        panel->AddText(state.displayFont, 9.0f, ImVec2(x, midY - labelExtent.y * 0.5f), withAlpha(kCpRed, 1.0f), label.c_str());
        const ImVec4& color = cyberpunkLoadColor(value, 0.50f, 0.80f);
        drawSlantedSegments(panel, Box{ImVec2(x + 28.0f, midY - 4.0f), ImVec2(x + columnWidth - 36.0f, midY + 4.0f)}, value, color);
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.textFont, 15.0f, text);
        panel->AddText(state.textFont, 15.0f, ImVec2(x + columnWidth - extent.x, midY - extent.y * 0.5f), withAlpha(value / 100.0f > 0.5f ? color : kCpText, 1.0f),
                       text.c_str());
    }
    ImGui::Dummy(room);
    ImGui::EndChild();
}

// The title in yellow over red and cyan echoes that jump apart when it
// glitches, "// NEURAL LINK DIAGNOSTICS", a blinking "REC" light, the clock in
// cyan, and a red rule with a notch underneath. Returns the buttons' rectangles.
HeaderButtons drawCyberpunkHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 4.0f;

    const bool glitching = state.animate && fmodf(state.time, 4.0f) < 0.12f;
    const float split = glitching ? 4.0f : 1.5f;
    const ImVec2 titleExtent = textSize(display, 22.0f, "VEEASTATS");
    const ImVec2 titlePos(box.min.x, std::floor(midY - titleExtent.y * 0.5f));
    draw->AddText(display, 22.0f, titlePos + ImVec2(-split, glitching ? 1.0f : 0.0f), withAlpha(kCpRed, 0.75f), "VEEASTATS");
    draw->AddText(display, 22.0f, titlePos + ImVec2(split, glitching ? -1.0f : 0.0f), withAlpha(kCpCyan, 0.55f), "VEEASTATS");
    draw->AddText(display, 22.0f, titlePos, withAlpha(kCpYellow, 1.0f), "VEEASTATS");
    const ImVec2 subtitleExtent = textSize(display, 10.0f, "// NEURAL LINK DIAGNOSTICS");
    draw->AddText(display, 10.0f, ImVec2(box.min.x + titleExtent.x + 16.0f, midY - subtitleExtent.y * 0.5f + 2.0f), withAlpha(kCpRed, 1.0f),
                  "// NEURAL LINK DIAGNOSTICS");

    const HeaderButtons buttons = drawHeaderButtons(ui, box.max.x, midY);
    float right = buttons.storage.min.x - 18.0f;

    std::string clock, date;
    currentClock(clock, date);
    const ImVec2 clockExtent = textSize(display, 16.0f, clock);
    right -= clockExtent.x;
    draw->AddText(display, 16.0f, ImVec2(right, midY - clockExtent.y * 0.5f), withAlpha(kCpCyan, 1.0f), clock.c_str());
    const ImVec2 dateExtent = textSize(state.textFont, 14.0f, date);
    right -= dateExtent.x + 12.0f;
    draw->AddText(state.textFont, 14.0f, ImVec2(right, midY - dateExtent.y * 0.5f), withAlpha(kCpRed, 0.9f), date.c_str());

    const bool lightOn = !state.animate || fmodf(state.time, 1.0f) < 0.6f;
    const ImVec2 recExtent = textSize(display, 9.0f, "REC");
    right -= recExtent.x + 22.0f;
    if (lightOn) draw->AddCircleFilled(ImVec2(right - 6.0f, midY), 3.5f, withAlpha(kCpHot, 1.0f));
    draw->AddCircle(ImVec2(right - 6.0f, midY), 5.5f, withAlpha(kCpHot, 0.6f));
    draw->AddText(display, 9.0f, ImVec2(right + 3.0f, midY - recExtent.y * 0.5f), withAlpha(kCpHot, 1.0f), "REC");

    // The rule: red, stepping down a few pixels a third of the way along.
    const float ruleY = std::floor(box.max.y) - 4.5f;
    const float step = box.min.x + std::floor(box.width() * 0.33f);
    const ImVec2 rule[] = {ImVec2(box.min.x, ruleY - 3.0f), ImVec2(step, ruleY - 3.0f), ImVec2(step + 3.0f, ruleY), ImVec2(box.max.x, ruleY)};
    draw->AddPolyline(rule, 4, withAlpha(kCpRed, 0.8f), ImDrawFlags_None, 1.0f);
    draw->AddRectFilled(ImVec2(box.min.x, ruleY - 5.0f), ImVec2(box.min.x + 60.0f, ruleY - 3.0f), withAlpha(kCpYellow, 1.0f));
    for (int i = 0; i < 5; ++i) {
        const float x = box.max.x - 8.0f - static_cast<float>(i) * 6.0f;
        draw->AddRectFilled(ImVec2(x - 3.0f, ruleY + 2.0f), ImVec2(x, ruleY + 5.0f), withAlpha(kCpRed, 0.7f));
    }
    return buttons;
}

void applyCyberpunkStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0.0f;
    style.ScrollbarRounding = style.GrabRounding = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 6.0f;

    auto red = [](float alpha) { return ImVec4(kCpRed.x, kCpRed.y, kCpRed.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kCpText;
    colors[ImGuiCol_TextDisabled]         = ImVec4(kCpRed.x, kCpRed.y, kCpRed.z, 0.80f);
    colors[ImGuiCol_WindowBg]             = ImVec4(0.05f, 0.015f, 0.025f, 0.97f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.05f, 0.015f, 0.025f, 0.97f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = red(0.60f);
    colors[ImGuiCol_FrameBg]              = red(0.08f);
    colors[ImGuiCol_FrameBgHovered]       = red(0.20f);
    colors[ImGuiCol_FrameBgActive]        = red(0.30f);
    colors[ImGuiCol_Button]               = red(0.14f);
    colors[ImGuiCol_ButtonHovered]        = red(0.28f);
    colors[ImGuiCol_ButtonActive]         = red(0.40f);
    colors[ImGuiCol_Header]               = red(0.22f);
    colors[ImGuiCol_HeaderHovered]        = red(0.32f);
    colors[ImGuiCol_HeaderActive]         = red(0.42f);
    colors[ImGuiCol_CheckMark]            = kCpCyan;
    colors[ImGuiCol_Separator]            = red(0.30f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = red(0.35f);
    colors[ImGuiCol_ScrollbarGrabHovered] = red(0.50f);
    colors[ImGuiCol_ScrollbarGrabActive]  = red(0.65f);
    colors[ImGuiCol_TextSelectedBg]       = red(0.30f);
    colors[ImGuiCol_NavCursor]            = kCpCyan;
}

// ---- Classroom (GNOME style) ----------------------------------------------------
// GNOME's own look (libadwaita): a plain window under a header bar, content on
// rounded cards in titled groups, thin progress rings and bars, and Inter (the
// typeface GNOME's Adwaita Sans is built on). Like a real GNOME app it follows
// the desktop: light or dark, in the user's accent colour. Section 8 reads
// those from the desktop and calls setGnomeAppearance().

// The colours of the current style.
struct GnomeColors {
    ImVec4 window, headerbar, card, popover;
    ImVec4 text, dim;      // ordinary and secondary text
    ImVec4 shade;          // borders, separators and empty tracks
    ImVec4 accent;         // fills
    bool dark{false};
};
GnomeColors g_gnome;

const ImVec4 kGnomeBlue  (0.208f, 0.518f, 0.894f, 1.0f);   // #3584e4, GNOME's default accent
const ImVec4 kGnomeYellow(0.898f, 0.647f, 0.039f, 1.0f);   // #e5a50a, "warning": busy
const ImVec4 kGnomeRed   (0.878f, 0.106f, 0.141f, 1.0f);   // #e01b24, "error": critical

ImVec4 rgbColor(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.0f, static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                  static_cast<float>(rgb & 0xff) / 255.0f, alpha);
}

// Switches between libadwaita's light and dark colours, in the given accent.
void setGnomeAppearance(bool dark, const ImVec4& accent) {
    GnomeColors& c = g_gnome;
    c.dark = dark;
    c.accent = accent;
    if (dark) {
        c.window = rgbColor(0x222226);
        c.headerbar = rgbColor(0x2e2e32);
        c.card = rgbColor(0x36363a);
        c.popover = rgbColor(0x36363a);
        c.text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        c.dim = ImVec4(1.0f, 1.0f, 1.0f, 0.55f);
        c.shade = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    } else {
        c.window = rgbColor(0xfafafb);
        c.headerbar = rgbColor(0xffffff);
        c.card = rgbColor(0xffffff);
        c.popover = rgbColor(0xffffff);
        c.text = ImVec4(0.0f, 0.0f, 0.024f, 0.80f);
        c.dim = ImVec4(0.0f, 0.0f, 0.024f, 0.55f);
        c.shade = ImVec4(0.0f, 0.0f, 0.024f, 0.10f);
    }
}

// The accent colour when relaxed, yellow when busy, red when critical.
ImVec4 gnomeLoadColor(float percent, float warnAt, float critAt) {
    const float fraction = percent / 100.0f;
    return (fraction > critAt) ? kGnomeRed : (fraction > warnAt) ? kGnomeYellow : g_gnome.accent;
}

// "OPERATING SYSTEM" -> "Operating system", the way GNOME writes headings.
// Acronyms such as "CPU" and anything with a digit stay as they are.
std::string gnomeCase(const std::string& text) {
    static const char* const kKeep[] = {"CPU", "GPU", "RAM", "VRAM", "VDDC", "OS", "GB", "N/A"};
    std::string result, word;
    const auto flush = [&] {
        bool keep = word.size() == 1;
        for (const char* acronym : kKeep) keep = keep || word == acronym;
        for (const char c : word) keep = keep || std::isdigit(static_cast<unsigned char>(c));
        if (word == "VCORE") word = "VCore";
        else if (!keep) for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        result += word;
        word.clear();
    };
    for (const char c : text) {
        if (c == ' ') {
            flush();
            result += ' ';
        } else {
            word += c;
        }
    }
    flush();
    if (!result.empty()) result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

constexpr float kGnomeHeaderBar = 58.0f;   // the header bar's height: the window's top padding plus the header row

// A rounded card, with a soft shadow in the light style and a hairline edge.
void drawGnomeCard(ImDrawList* draw, const Box& box) {
    constexpr float kRounding = 12.0f;
    if (!g_gnome.dark) draw->AddRectFilled(box.min + ImVec2(0, 1), box.max + ImVec2(0, 2), withAlpha(ImVec4(0, 0, 0, 1), 0.06f), kRounding);
    draw->AddRectFilled(box.min, box.max, withAlpha(g_gnome.card, 1.0f), kRounding);
    draw->AddRect(box.min, box.max, withAlpha(g_gnome.shade, 1.0f), kRounding);
}

// A thin rounded bar, like GNOME's progress bars.
void drawGnomeBar(ImDrawList* draw, const Box& box, float percent, const ImVec4& color) {
    const float rounding = box.height() * 0.5f;
    draw->AddRectFilled(box.min, box.max, withAlpha(g_gnome.shade, 1.0f), rounding);
    const float fill = std::floor(box.width() * std::clamp(percent, 0.0f, 100.0f) / 100.0f);
    if (fill >= 1.0f) draw->AddRectFilled(box.min, ImVec2(box.min.x + std::max(fill, box.height()), box.max.y), withAlpha(color, 1.0f), rounding);
}

// The window colour, with the header bar across the top and a hairline under it.
void drawGnomeBackground(const ImVec2& size, const HudState&) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, withAlpha(g_gnome.window, 1.0f));
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(size.x, kGnomeHeaderBar), withAlpha(g_gnome.headerbar, 1.0f));
    draw->AddLine(ImVec2(0.0f, kGnomeHeaderBar - 0.5f), ImVec2(size.x, kGnomeHeaderBar - 0.5f), withAlpha(g_gnome.shade, 1.0f));
}

// A group: its title in bold above a rounded card. Returns the area for content.
Box drawGnomePanelFrame(ImDrawList* draw, const Box& box, const std::string& title, const HudState& state) {
    constexpr float kTitleHeight = 24.0f;
    draw->AddText(state.displayFont, 14.0f, ImVec2(box.min.x + 4.0f, box.min.y + 2.0f), withAlpha(g_gnome.text, 1.0f), gnomeCase(title).c_str());
    const Box card{ImVec2(box.min.x, box.min.y + kTitleHeight), box.max};
    drawGnomeCard(draw, card);
    return Box{card.min + ImVec2(14.0f, 10.0f), card.max - ImVec2(12.0f, 8.0f)};
}

// "Label   value" in a card, with a hairline between rows (as in GNOME's lists).
void drawGnomeInfoRow(const HudState&, const char* label, const std::string& value) {
    ImGui::TableNextRow();
    if (ImGui::TableGetRowIndex() > 0 && label[0] != '\0') {
        const float y = std::floor(ImGui::GetCursorScreenPos().y) - 1.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 4.0f, y),
                                            withAlpha(g_gnome.shade, 1.0f));
    }
    ImGui::TableNextColumn();
    ImGui::TextColored(g_gnome.dim, "%s", gnomeCase(label).c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

// One gauge as a card: a bold heading, a thin ring that fills clockwise from
// the top with the percentage in the middle, and the readouts under it.
void drawGnomeGaugeCard(ImDrawList* draw, const Box& box, const GaugeReading& reading, const HudState& state) {
    drawGnomeCard(draw, box);
    draw->AddText(state.displayFont, 15.0f, ImVec2(box.min.x + 16.0f, box.min.y + 12.0f), withAlpha(g_gnome.text, 1.0f), gnomeCase(reading.label).c_str());

    constexpr float kThickness = 8.0f, kBelow = 60.0f;   // kBelow: room for the readouts, keeping them as far from the card's edge as the heading is
    const float radius = std::floor(std::min(box.width() * 0.30f, (box.height() - 38.0f - kBelow) * 0.5f));
    const ImVec2 center(std::floor((box.min.x + box.max.x) * 0.5f), std::floor(box.min.y + 38.0f + radius));
    draw->AddCircle(center, radius, withAlpha(g_gnome.shade, 1.0f), 64, kThickness);
    const float fraction = reading.hasValue ? std::clamp(reading.percent, 0.0f, 100.0f) / 100.0f : 0.0f;
    if (fraction > 0.0f) {
        const ImVec4 color = gnomeLoadColor(reading.percent, reading.warnAt, reading.critAt);
        const float start = -kPi * 0.5f, end = start + 2.0f * kPi * fraction;
        draw->PathArcTo(center, radius, start, end, 64);
        draw->PathStroke(withAlpha(color, 1.0f), kThickness);
        draw->AddCircleFilled(pointOnCircle(center, radius, start), kThickness * 0.5f, withAlpha(color, 1.0f));   // round ends
        draw->AddCircleFilled(pointOnCircle(center, radius, end), kThickness * 0.5f, withAlpha(color, 1.0f));
    }
    const std::string number = reading.hasValue ? format("%.0f%%", std::clamp(reading.percent, 0.0f, 100.0f)) : "N/A";
    drawCenteredText(draw, state.displayFont, std::round(radius * 0.42f), center, withAlpha(reading.hasValue ? g_gnome.text : g_gnome.dim, 1.0f), number);

    drawCenteredText(draw, state.textFont, 15.0f, ImVec2(center.x, center.y + radius + 20.0f), withAlpha(g_gnome.text, 1.0f), reading.line1);
    drawCenteredText(draw, state.textFont, 13.0f, ImVec2(center.x, center.y + radius + 38.0f), withAlpha(g_gnome.dim, 1.0f), gnomeCase(reading.line2));
}

void drawGnomeGauges(ImDrawList* draw, const Box& box, const HudState& state, const LiveStats& live) {
    const std::array<GaugeReading, 4> readings = gaugeReadings(state, live);
    constexpr float kGap = 12.0f;
    const float width = std::floor((box.width() - kGap * 3.0f) / 4.0f);
    for (int i = 0; i < 4; ++i) {
        const float x = box.min.x + static_cast<float>(i) * (width + kGap);
        drawGnomeGaugeCard(draw, Box{ImVec2(x, box.min.y), ImVec2(x + width, box.max.y)}, readings[i], state);
    }
}

// The cores as rows of "CPU 0   ▬▬▬   34%", in as many columns as it takes.
void drawGnomeCores(ImDrawList* draw, const Box& box, const HudState& state) {
    beginHudPanel(drawGnomePanelFrame(draw, box, format("Processor \xC2\xB7 %zu threads", state.cores.size()), state), "##GnomeCores");
    ImDrawList* panel = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();

    constexpr float kRowHeight = 19.0f, kColumnGap = 20.0f;
    const int count = static_cast<int>(state.cores.size());
    const int rowsThatFit = std::max(static_cast<int>(room.y / kRowHeight), 1);
    const int columns = std::max((count + rowsThatFit - 1) / rowsThatFit, 1);
    const int rows = (count + columns - 1) / columns;
    const float columnWidth = (room.x - kColumnGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (int i = 0; i < count; ++i) {
        const float value = std::clamp(state.cores[static_cast<size_t>(i)], 0.0f, 100.0f);
        const float x = std::floor(start.x + static_cast<float>(i / rows) * (columnWidth + kColumnGap));
        const float midY = std::floor(start.y + (static_cast<float>(i % rows) + 0.5f) * kRowHeight);
        const std::string label = format("CPU %d", state.coreId(i));
        const ImVec2 labelExtent = textSize(state.textFont, 13.0f, label);
        panel->AddText(state.textFont, 13.0f, ImVec2(x, midY - labelExtent.y * 0.5f), withAlpha(g_gnome.dim, 1.0f), label.c_str());
        drawGnomeBar(panel, Box{ImVec2(x + 50.0f, midY - 3.0f), ImVec2(x + columnWidth - 40.0f, midY + 3.0f)}, value, gnomeLoadColor(value, 0.50f, 0.80f));
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.textFont, 13.0f, text);
        panel->AddText(state.textFont, 13.0f, ImVec2(x + columnWidth - extent.x, midY - extent.y * 0.5f), withAlpha(g_gnome.text, 1.0f), text.c_str());
    }
    ImGui::Dummy(room);
    ImGui::EndChild();
}

// Like a GNOME header bar: the title and a dim subtitle in the middle, a live
// light with the date and time on the left, the buttons on the right.
HeaderButtons drawGnomeHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 3.0f;
    const float centerX = std::floor((box.min.x + box.max.x) * 0.5f);
    drawCenteredText(draw, state.displayFont, 15.0f, ImVec2(centerX, midY - 7.0f), withAlpha(g_gnome.text, 1.0f), "VeeaStats");
    drawCenteredText(draw, state.textFont, 12.0f, ImVec2(centerX, midY + 9.0f), withAlpha(g_gnome.dim, 1.0f), "System Monitor");

    const float pulse = state.animate ? 0.55f + 0.45f * sinf(state.time * 2.5f) : 1.0f;
    draw->AddCircleFilled(ImVec2(box.min.x + 8.0f, midY), 4.5f, withAlpha(g_gnome.accent, pulse));
    char when[48] = "";
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(when, sizeof(when), "%a %e %b  %H:%M", &local);
    const ImVec2 extent = textSize(state.textFont, 14.0f, when);
    draw->AddText(state.textFont, 14.0f, ImVec2(box.min.x + 20.0f, midY - extent.y * 0.5f), withAlpha(g_gnome.text, 1.0f), when);

    return drawHeaderButtons(ui, box.max.x, midY);
}

void applyGnomeStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowRounding = style.PopupRounding = 12.0f;
    style.FrameRounding = style.GrabRounding = style.ScrollbarRounding = 6.0f;
    style.ChildRounding = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ScrollbarSize = 8.0f;

    const GnomeColors& c = g_gnome;
    const auto tint = [&](float alpha) { return c.dark ? ImVec4(1.0f, 1.0f, 1.0f, alpha) : ImVec4(0.0f, 0.0f, 0.024f, alpha); };
    const auto accent = [&](float alpha) { return ImVec4(c.accent.x, c.accent.y, c.accent.z, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = c.text;
    colors[ImGuiCol_TextDisabled]         = c.dim;
    colors[ImGuiCol_WindowBg]             = c.popover;
    colors[ImGuiCol_PopupBg]              = c.popover;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = tint(0.15f);
    colors[ImGuiCol_FrameBg]              = tint(0.08f);
    colors[ImGuiCol_FrameBgHovered]       = tint(0.12f);
    colors[ImGuiCol_FrameBgActive]        = tint(0.18f);
    colors[ImGuiCol_Button]               = tint(0.08f);
    colors[ImGuiCol_ButtonHovered]        = tint(0.12f);
    colors[ImGuiCol_ButtonActive]         = tint(0.18f);
    colors[ImGuiCol_Header]               = accent(0.22f);
    colors[ImGuiCol_HeaderHovered]        = tint(0.08f);
    colors[ImGuiCol_HeaderActive]         = accent(0.30f);
    colors[ImGuiCol_CheckMark]            = c.accent;
    colors[ImGuiCol_Separator]            = c.shade;
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]        = tint(0.20f);
    colors[ImGuiCol_ScrollbarGrabHovered] = tint(0.30f);
    colors[ImGuiCol_ScrollbarGrabActive]  = tint(0.40f);
    colors[ImGuiCol_TextSelectedBg]       = accent(0.30f);
    colors[ImGuiCol_NavCursor]            = c.accent;
}

// ============================================================================
// 7. SKIN SWITCHING
// ============================================================================

// Every font the skins use, added once at startup.
struct UiFonts {
    ImFont* classic{nullptr};           // ImGui's built-in ProggyClean, 13 px (the original look)
    ImFont* jarvText{nullptr};          // Rajdhani
    ImFont* jarvDisplay{nullptr};       // Orbitron
    ImFont* galacticText{nullptr};      // Saira Semi Condensed
    ImFont* galacticDisplay{nullptr};   // Michroma
    ImFont* gunshipText{nullptr};       // Tiny5: crisp at multiples of 8 px
    ImFont* gunshipDisplay{nullptr};    // Press Start 2P: also crisp at multiples of 8 px
    ImFont* halloweenText{nullptr};     // Fredoka SemiBold
    ImFont* halloweenDisplay{nullptr};  // Henny Penny
    ImFont* poseidonText{nullptr};      // Marcellus
    ImFont* poseidonDisplay{nullptr};   // Cinzel Bold
    ImFont* otakuText{nullptr};         // M PLUS Rounded 1c ExtraBold
    ImFont* otakuDisplay{nullptr};      // Dela Gothic One
    ImFont* cyberpunkText{nullptr};     // Rajdhani (the same font as JARV's text)
    ImFont* cyberpunkDisplay{nullptr};  // Chakra Petch Bold
    ImFont* gnomeText{nullptr};         // Inter
    ImFont* gnomeDisplay{nullptr};      // Inter Bold
};

UiFonts loadFonts() {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    UiFonts fonts;
    fonts.classic         = atlas->AddFontDefaultBitmap();
    fonts.jarvText        = atlas->AddFontFromMemoryCompressedBase85TTF(RajdhaniSemiBold_compressed_data_base85, 17.0f);
    fonts.jarvDisplay     = atlas->AddFontFromMemoryCompressedBase85TTF(OrbitronSemiBold_compressed_data_base85, 13.0f);
    fonts.galacticText    = atlas->AddFontFromMemoryCompressedBase85TTF(SairaSemiCondensedMedium_compressed_data_base85, 16.0f);
    fonts.galacticDisplay = atlas->AddFontFromMemoryCompressedBase85TTF(MichromaRegular_compressed_data_base85, 13.0f);

    // Pixel fonts: no oversampling and whole-pixel positions keep their edges sharp.
    ImFontConfig pixel;
    pixel.OversampleH = pixel.OversampleV = 1;
    pixel.PixelSnapH = true;
    fonts.gunshipText    = atlas->AddFontFromMemoryCompressedBase85TTF(Tiny5Regular_compressed_data_base85, 16.0f, &pixel);
    fonts.gunshipDisplay = atlas->AddFontFromMemoryCompressedBase85TTF(PressStart2PRegular_compressed_data_base85, 16.0f, &pixel);

    fonts.halloweenText    = atlas->AddFontFromMemoryCompressedBase85TTF(FredokaSemiBold_compressed_data_base85, 16.0f);
    fonts.halloweenDisplay = atlas->AddFontFromMemoryCompressedBase85TTF(HennyPennyRegular_compressed_data_base85, 18.0f);

    fonts.poseidonText     = atlas->AddFontFromMemoryCompressedBase85TTF(MarcellusRegular_compressed_data_base85, 16.0f);
    fonts.poseidonDisplay  = atlas->AddFontFromMemoryCompressedBase85TTF(CinzelBold_compressed_data_base85, 14.0f);
    fonts.otakuText        = atlas->AddFontFromMemoryCompressedBase85TTF(MPlusRounded1cExtraBold_compressed_data_base85, 15.0f);
    fonts.otakuDisplay     = atlas->AddFontFromMemoryCompressedBase85TTF(DelaGothicOneRegular_compressed_data_base85, 16.0f);
    fonts.cyberpunkText    = fonts.jarvText;
    fonts.cyberpunkDisplay = atlas->AddFontFromMemoryCompressedBase85TTF(ChakraPetchBold_compressed_data_base85, 14.0f);
    fonts.gnomeText        = atlas->AddFontFromMemoryCompressedBase85TTF(InterRegular_compressed_data_base85, 15.0f);
    fonts.gnomeDisplay     = atlas->AddFontFromMemoryCompressedBase85TTF(InterBold_compressed_data_base85, 15.0f);
    return fonts;
}

// Every HUD skin, ready to draw (see section 6).
struct HudSkins {
    HudSkin jarv, galactic, gunship, halloween, poseidon, otaku, cyberpunk, gnome;

    const HudSkin& forSkin(Skin skin) const {
        switch (skin) {
            case Skin::Galactic:  return galactic;
            case Skin::Gunship:   return gunship;
            case Skin::Halloween: return halloween;
            case Skin::Poseidon:  return poseidon;
            case Skin::Otaku:     return otaku;
            case Skin::Cyberpunk: return cyberpunk;
            case Skin::Gnome:     return gnome;
            default:              return jarv;
        }
    }
};

HudSkins makeHudSkins(const UiFonts& fonts) {
    HudSkins skins;
    skins.jarv = {fonts.jarvText, fonts.jarvDisplay, 10.0f, {64.0f, 96.0f, 76.0f}, drawJarvBackground, drawJarvHeader,
                  drawJarvGauges, drawJarvPanelFrame, drawJarvCores, drawJarvInfoRow, applyJarvStyle};
    skins.galactic = {fonts.galacticText, fonts.galacticDisplay, 9.0f, {}, drawGalacticBackground, drawGalacticHeader,
                      drawGalacticGauges, drawGalacticPanelFrame, drawGalacticCores, drawGalacticInfoRow, applyGalacticStyle};
    skins.gunship = {fonts.gunshipText, fonts.gunshipDisplay, 8.0f, {}, drawGunshipBackground, drawGunshipHeader,
                     drawGunshipGauges, drawGunshipPanelFrame, drawGunshipCores, drawGunshipInfoRow, applyGunshipStyle};
    skins.halloween = {fonts.halloweenText, fonts.halloweenDisplay, 13.0f, {}, drawHalloweenBackground, drawHalloweenHeader,
                       drawHalloweenGauges, drawHalloweenPanelFrame, drawHalloweenCores, drawHalloweenInfoRow, applyHalloweenStyle,
                       fonts.halloweenText};
    skins.poseidon = {fonts.poseidonText, fonts.poseidonDisplay, 10.0f, {}, drawPoseidonBackground, drawPoseidonHeader,
                      drawPoseidonGauges, drawPoseidonPanelFrame, drawPoseidonCores, drawPoseidonInfoRow, applyPoseidonStyle};
    skins.otaku = {fonts.otakuText, fonts.otakuDisplay, 12.0f, {}, drawOtakuBackground, drawOtakuHeader,
                   drawOtakuGauges, drawOtakuPanelFrame, drawOtakuCores, drawOtakuInfoRow, applyOtakuStyle, fonts.otakuText};
    skins.cyberpunk = {fonts.cyberpunkText, fonts.cyberpunkDisplay, 10.0f, {}, drawCyberpunkBackground, drawCyberpunkHeader,
                       drawCyberpunkGauges, drawCyberpunkPanelFrame, drawCyberpunkCores, drawCyberpunkInfoRow, applyCyberpunkStyle};
    skins.gnome = {fonts.gnomeText, fonts.gnomeDisplay, 14.0f, {}, drawGnomeBackground, drawGnomeHeader,
                   drawGnomeGauges, drawGnomePanelFrame, drawGnomeCores, drawGnomeInfoRow, applyGnomeStyle, fonts.gnomeText};
    return skins;
}

// Switches ImGui's colours, spacing and default font to the given skin.
// Called between frames, never in the middle of one.
void applySkin(Skin skin, const UiFonts& fonts) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();   // start from the defaults so nothing carries over from the previous skin
    ImGui::StyleColorsDark();
    ImFont* font = fonts.classic;
    switch (skin) {
        case Skin::Classic:  break;
        case Skin::Jarv:     applyJarvStyle(style);     font = fonts.jarvText;     break;
        case Skin::Galactic: applyGalacticStyle(style); font = fonts.galacticText; break;
        case Skin::Gunship:  applyGunshipStyle(style);  font = fonts.gunshipText;  break;
        case Skin::Halloween: applyHalloweenStyle(style); font = fonts.halloweenText; break;
        case Skin::Poseidon:  applyPoseidonStyle(style);  font = fonts.poseidonText;  break;
        case Skin::Otaku:     applyOtakuStyle(style);     font = fonts.otakuText;     break;
        case Skin::Cyberpunk: applyCyberpunkStyle(style); font = fonts.cyberpunkText; break;
        case Skin::Gnome:     applyGnomeStyle(style);     font = fonts.gnomeText;     break;
    }
    ImGui::GetIO().FontDefault = font;
}

// Draws the whole window in the chosen skin.
void drawWindow(const ImVec2& size, UiState& ui, const HudSkins& hudSkins, HudState& hud, const StaticInfo& info,
                const LiveStats& live) {
    if (ui.settings.skin == Skin::Classic) drawDashboard(size, ui, info, live);
    else                                   drawHudDashboard(size, ui, hudSkins.forSkin(ui.settings.skin), hud, info, live);
}

void onGlfwError(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// The main loop sleeps between updates and should only wake early when the
// user does something. Waking early is not proof of that on its own: Wayland
// also wakes the program with housekeeping messages after every frame. So these
// callbacks flag real input (mouse, keyboard, resize, focus, redraw requests)
// in the bool that watchForInput() is given.
void markInput(GLFWwindow* window) {
    *static_cast<bool*>(glfwGetWindowUserPointer(window)) = true;
}

// Must be called before ImGui installs its own callbacks, which then pass every
// event on to these.
void watchForInput(GLFWwindow* window, bool* inputArrived) {
    glfwSetWindowUserPointer(window, inputArrived);
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double, double) { markInput(w); });
    glfwSetCursorEnterCallback(window, [](GLFWwindow* w, int) { markInput(w); });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int, int, int) { markInput(w); });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double, double) { markInput(w); });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int, int, int, int) { markInput(w); });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int) { markInput(w); });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int) { markInput(w); });
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) { markInput(w); });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) { markInput(w); });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* w, int) { markInput(w); });
}

// ============================================================================
// 8. OVERLAY, HOTKEY AND BACKGROUND MODE
// ============================================================================
//
// Ctrl+Shift+O shows a small see-through panel with the main numbers on top of
// whatever the user is doing, usually a game.
//
// Wayland doesn't let ordinary windows sit on top of other programs, so the
// overlay is an X11 window instead (through XWayland on Wayland desktops). An
// "override-redirect" X11 window is drawn above everything, fullscreen games
// included, and never takes focus. GLFW talks to only one display system per
// process, so the overlay runs as a second copy of this program
// (`CpuMonitor --overlay`) that is sent the numbers through a pipe.
//
// The hotkey comes from two places, because neither covers everything:
//   - The desktop's GlobalShortcuts portal (GNOME 48+, KDE Plasma 6) works in
//     every program. The desktop asks the user once to confirm the shortcut.
//   - An X11 key grab in the overlay process works whenever an X11 program has
//     focus, on any desktop. Proton/Wine games are X11 programs, and on X11
//     sessions everything is.
//
// libX11 and GLib are loaded when first needed (the way GLFW loads its own
// libraries), so a system without them still runs VeeaStats, just without the
// overlay or the hotkey.

// Loads `name` from `library` into `function`, keeping the function's real type.
template <typename Function>
bool loadSymbol(void* library, const char* name, Function& function) {
    function = reinterpret_cast<Function>(dlsym(library, name));
    return function != nullptr;
}

// ---- Requests from other threads --------------------------------------------

// Other threads ask the main loop to do things by setting these bits and waking
// it up: the overlay's report reader (its hotkey, on screen or not, ended), the
// hotkey portal, the desktop-appearance watcher, the listener for a second
// launch of the program, and the threads reading the hardware details and the
// storage usage.
constexpr unsigned kRequestShowWindow     = 1u << 0;
constexpr unsigned kRequestQuit           = 1u << 1;
constexpr unsigned kRequestToggleOverlay  = 1u << 2;
constexpr unsigned kRequestOverlayChanged = 1u << 3;   // it appeared or disappeared; see g_overlayActivePid
constexpr unsigned kRequestStaticInfo     = 1u << 4;   // the hardware details are ready (see main)
constexpr unsigned kRequestAppearance     = 1u << 5;   // the desktop's light/dark style or accent colour changed
constexpr unsigned kRequestOverlayEnded   = 1u << 6;   // the overlay process stopped (crashed, or lost its display)
constexpr unsigned kRequestRedraw         = 1u << 7;   // something on screen changed (storage numbers arrived)
std::atomic<unsigned> g_requests{0};

// The overlay process that is on screen right now (not one waiting, hidden, for
// its app to be focused again), or -1. The main loop only reads stats while
// someone can see them. A pid rather than a flag, so a report from an overlay
// that has since been replaced can't change it.
std::atomic<pid_t> g_overlayActivePid{-1};

// The overlay process whose reports just ended (it quit or crashed), or -1.
std::atomic<pid_t> g_overlayEndedPid{-1};

// The helper threads run until the process ends, so a request can arrive while
// main() is shutting GLFW down. GLFW may only be woken while it is running.
std::mutex g_glfwLock;
bool g_glfwRunning = false;   // guarded by g_glfwLock

void postRequest(unsigned request) {
    g_requests |= request;
    std::lock_guard<std::mutex> lock(g_glfwLock);
    if (g_glfwRunning) glfwPostEmptyEvent();   // wakes the main loop if it is sleeping
}

void wakeMainLoop() { postRequest(kRequestRedraw); }

// ---- The overlay process ------------------------------------------------------

// The libX11 functions the overlay uses.
struct X11Api {
    decltype(&XInitThreads) initThreads{};
    decltype(&XOpenDisplay) openDisplay{};
    decltype(&XDefaultRootWindow) defaultRootWindow{};
    decltype(&XKeysymToKeycode) keysymToKeycode{};
    decltype(&XGrabKey) grabKey{};
    decltype(&XUngrabKey) ungrabKey{};
    decltype(&XRefreshKeyboardMapping) refreshKeyboardMapping{};
    decltype(&XkbSetDetectableAutoRepeat) setDetectableAutoRepeat{};
    decltype(&XSync) sync{};
    decltype(&XNextEvent) nextEvent{};
    decltype(&XSetErrorHandler) setErrorHandler{};
    decltype(&XInternAtom) internAtom{};
    decltype(&XGetWindowProperty) getWindowProperty{};
    decltype(&XFree) free{};
    decltype(&XGetWindowAttributes) getWindowAttributes{};
    decltype(&XTranslateCoordinates) translateCoordinates{};
    decltype(&XChangeWindowAttributes) changeWindowAttributes{};
    decltype(&XMoveWindow) moveWindow{};

    bool load() {
        void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        return library && loadSymbol(library, "XInitThreads", initThreads) && loadSymbol(library, "XOpenDisplay", openDisplay) &&
               loadSymbol(library, "XDefaultRootWindow", defaultRootWindow) &&
               loadSymbol(library, "XKeysymToKeycode", keysymToKeycode) && loadSymbol(library, "XGrabKey", grabKey) &&
               loadSymbol(library, "XUngrabKey", ungrabKey) && loadSymbol(library, "XRefreshKeyboardMapping", refreshKeyboardMapping) &&
               loadSymbol(library, "XkbSetDetectableAutoRepeat", setDetectableAutoRepeat) &&
               loadSymbol(library, "XSync", sync) && loadSymbol(library, "XNextEvent", nextEvent) &&
               loadSymbol(library, "XSetErrorHandler", setErrorHandler) && loadSymbol(library, "XInternAtom", internAtom) &&
               loadSymbol(library, "XGetWindowProperty", getWindowProperty) && loadSymbol(library, "XFree", free) &&
               loadSymbol(library, "XGetWindowAttributes", getWindowAttributes) &&
               loadSymbol(library, "XTranslateCoordinates", translateCoordinates) &&
               loadSymbol(library, "XChangeWindowAttributes", changeWindowAttributes) &&
               loadSymbol(library, "XMoveWindow", moveWindow);
    }
};

// X11 reports errors (for example, another program already owns Ctrl+Shift+O)
// to this handler. The default one exits the program; none of them is worth that.
int ignoreX11Error(Display*, XErrorEvent*) {
    return 0;
}

// The numbers the overlay shows, sent over by the main process as one line:
// "stats <cpu %> <cpu V> <cpu °C> <ram %> <ram used GB> <ram total GB> <gpu has load> <gpu %> <gpu V> <gpu °C> <in °F>".
struct OverlayNumbers {
    float cpuPercent{0.0f}, cpuVolts{0.0f}, cpuCelsius{std::numeric_limits<float>::quiet_NaN()};   // NaN: no sensor (sent as "nan")
    float ramPercent{0.0f}, ramUsedGb{0.0f}, ramTotalGb{0.0f};
    int gpuHasLoad{0};
    float gpuPercent{0.0f}, gpuVolts{0.0f}, gpuCelsius{std::numeric_limits<float>::quiet_NaN()};
    int fahrenheit{0};
};

std::string overlayStatsLine(const LiveStats& live, bool fahrenheit) {
    return format("stats %.1f %.3f %.1f %.1f %.2f %.2f %d %.1f %.3f %.1f %d\n", averageCpuPercent(live), live.cpuVoltage,
                  live.cpuTemperatureC, live.ram.percent, live.ram.usedGb, live.ram.totalGb, live.gpu.hasLoad ? 1 : 0,
                  live.gpu.gpuUsage, live.gpu.voltageV, live.gpu.temperatureC, fahrenheit ? 1 : 0);
}

bool parseOverlayStatsLine(const std::string& line, OverlayNumbers& numbers) {
    OverlayNumbers parsed;
    if (sscanf(line.c_str(), "stats %f %f %f %f %f %f %d %f %f %f %d", &parsed.cpuPercent, &parsed.cpuVolts, &parsed.cpuCelsius,
               &parsed.ramPercent, &parsed.ramUsedGb, &parsed.ramTotalGb, &parsed.gpuHasLoad, &parsed.gpuPercent, &parsed.gpuVolts,
               &parsed.gpuCelsius, &parsed.fahrenheit) != 11) {
        return false;
    }
    numbers = parsed;
    return true;
}

// Watches for Ctrl+Shift+O on an X11 connection of its own (it waits in
// XNextEvent, which would hold up the drawing thread's calls on a shared one)
// and tells the main process "hotkey" on every press.
// Runs for the life of the overlay process.
void watchX11Hotkey(const X11Api* x11) {
    Display* display = x11->openDisplay(nullptr);
    if (!display) return;
    const Window root = x11->defaultRootWindow(display);
    constexpr unsigned kHotkeyModifiers = ControlMask | ShiftMask;

    // Holding the keys down makes X repeat the press. With "detectable
    // auto-repeat" the repeats come without a key release in between, so a
    // press counts only once the key has really been let go. (Without it, each
    // repeat comes as a release + press, and only the time between them tells.)
    Bool detectableRepeat = False;
    x11->setDetectableAutoRepeat(display, True, &detectableRepeat);

    // Grabs Ctrl+Shift+O, or moves the grab when the keyboard layout changes.
    // A grab only matches the exact modifiers, so it's made again with Caps Lock
    // and Num Lock on.
    KeyCode grabbed = 0;
    const auto grabHotkey = [&] {
        constexpr unsigned kLocks[] = {0u, unsigned(LockMask), unsigned(Mod2Mask), unsigned(LockMask | Mod2Mask)};
        if (grabbed != 0) {
            for (const unsigned locks : kLocks) x11->ungrabKey(display, grabbed, kHotkeyModifiers | locks, root);
        }
        // Keycode 0 (no "o" anywhere in the layout, as in a Cyrillic-only one)
        // means "any key" to XGrabKey: that would take every Ctrl+Shift shortcut
        // from every program. No hotkey is better.
        grabbed = x11->keysymToKeycode(display, XK_o);
        if (grabbed != 0) {
            for (const unsigned locks : kLocks) {
                x11->grabKey(display, grabbed, kHotkeyModifiers | locks, root, False, GrabModeAsync, GrabModeAsync);
            }
        }
        x11->sync(display, False);
    };
    grabHotkey();

    constexpr Time kRepeatGapMs = 500;   // only used without detectable auto-repeat
    bool released = true;
    Time lastPress = 0;
    XEvent event;
    while (true) {
        x11->nextEvent(display, &event);
        if (event.type == MappingNotify) {   // the keyboard layout changed: "o" may be another key now
            x11->refreshKeyboardMapping(&event.xmapping);
            if (event.xmapping.request == MappingKeyboard) grabHotkey();
            continue;
        }
        // While the grab is active, every key comes here: only O itself counts.
        if ((event.type != KeyPress && event.type != KeyRelease) || event.xkey.keycode != grabbed) continue;
        if (event.type == KeyRelease) {
            released = true;
            continue;
        }
        const bool repeat = !released || (!detectableRepeat && lastPress != 0 && event.xkey.time - lastPress < kRepeatGapMs);
        released = false;
        lastPress = event.xkey.time;
        if ((event.xkey.state & kHotkeyModifiers) != kHotkeyModifiers) continue;   // Ctrl or Shift was let go first
        if (!repeat && write(STDOUT_FILENO, "hotkey\n", 7) < 0) return;
    }
}

constexpr float kOverlayMargin = 12.0f;   // gap between the window's corner and the overlay

// The focused window, if it's one the overlay can sit on: an X11 window the user
// works in (Proton/Wine games are X11). None while a Wayland window has focus:
// Wayland doesn't tell other programs about its windows, and GNOME then reports
// a hidden 1x1 helper window as the active X11 window.
Window focusedX11Window(const X11Api& x11, Display* display, Window overlayWindow) {
    static const Atom activeWindowAtom = x11.internAtom(display, "_NET_ACTIVE_WINDOW", False);
    Window active = None;
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    if (x11.getWindowProperty(display, x11.defaultRootWindow(display), activeWindowAtom, 0, 1, False, XA_WINDOW, &type,
                              &format, &count, &remaining, &data) == Success && data) {
        // Any X client can set this property, in any shape: only a single
        // 32-bit window id is read (anything else would be read past its end).
        if (type == XA_WINDOW && format == 32 && count == 1) active = *reinterpret_cast<Window*>(data);
        x11.free(data);
    }

    XWindowAttributes attributes;
    constexpr int kSmallestWindow = 100;   // anything smaller is a helper, not something the user works in
    const bool usable = active != None && active != overlayWindow && x11.getWindowAttributes(display, active, &attributes) &&
                        attributes.map_state == IsViewable && !attributes.override_redirect &&
                        attributes.width >= kSmallestWindow && attributes.height >= kSmallestWindow;
    return usable ? active : None;
}

// Top-left corner of `window` on the screen, or nothing if the window no longer
// exists (its program has closed).
std::optional<ImVec2> windowCorner(const X11Api& x11, Display* display, Window window) {
    XWindowAttributes attributes;
    int x = 0, y = 0;
    Window child;
    if (!x11.getWindowAttributes(display, window, &attributes) ||
        !x11.translateCoordinates(display, window, x11.defaultRootWindow(display), 0, 0, &x, &y, &child)) {
        return std::nullopt;
    }
    return ImVec2(static_cast<float>(x), static_cast<float>(y));
}

// Top-left corner of the main screen's usable area, below GNOME's top bar or any
// other panel. GNOME lists each screen's usable area in _GTK_WORKAREAS_D0; other
// desktops give one area covering all screens in _NET_WORKAREA. (GLFW's
// glfwGetMonitorWorkarea also needs _NET_CURRENT_DESKTOP, which GNOME doesn't set.)
ImVec2 screenCorner(const X11Api& x11, Display* display) {
    // No monitor at all for a moment (the only screen went to sleep, a KVM
    // switch): GLFW has nothing to ask, and would crash if asked anyway.
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return ImVec2(0.0f, 0.0f);
    int monitorX = 0, monitorY = 0;
    glfwGetMonitorPos(monitor, &monitorX, &monitorY);
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    const long monitorRight = monitorX + (mode ? mode->width : 0), monitorBottom = monitorY + (mode ? mode->height : 0);

    for (const char* property : {"_GTK_WORKAREAS_D0", "_NET_WORKAREA"}) {
        Atom type;
        int format;
        unsigned long count, remaining;
        unsigned char* data = nullptr;
        const Atom atom = x11.internAtom(display, property, True);   // True: look up only, don't create it
        if (atom == None ||
            x11.getWindowProperty(display, x11.defaultRootWindow(display), atom, 0, 64, False, AnyPropertyType, &type, &format,
                                  &count, &remaining, &data) != Success || !data) {
            continue;
        }
        // Rectangles as (x, y, width, height); use the first that overlaps the main screen.
        const long* rects = reinterpret_cast<const long*>(data);
        std::optional<ImVec2> corner;
        for (unsigned long i = 0; i + 3 < count && format == 32 && !corner; i += 4) {
            const long left = std::max<long>(rects[i], monitorX), top = std::max<long>(rects[i + 1], monitorY);
            const long right = std::min<long>(rects[i] + rects[i + 2], monitorRight);
            const long bottom = std::min<long>(rects[i + 1] + rects[i + 3], monitorBottom);
            if (left < right && top < bottom) corner = ImVec2(static_cast<float>(left), static_cast<float>(top));
        }
        x11.free(data);
        if (corner) return *corner;
    }
    return ImVec2(static_cast<float>(monitorX), static_cast<float>(monitorY));
}

// The overlay is drawn in the active skin's style, at the window's top-left.
// Each look returns the panel's size, and the window is fitted to it:
//   CPU  34%  1.208 V
//   RAM  41%  12.5 / 30.5 GB
//   GPU  67%  58°C  0.812 V
struct OverlayRow {
    const char* label;
    bool hasPercent;
    float percent, warnAt, critAt;
    std::string detail;
    bool detailIsVoltage;   // shown in the voltage colour (Classic)
};

// "62°C  1.212 V": the temperature and voltage, whichever are known, or "N/A".
std::string overlaySensorText(float celsius, float volts, bool fahrenheit) {
    std::string text = hasTemperature(celsius) ? temperatureText(celsius, fahrenheit) : "";
    if (volts > 0.0f) text += (text.empty() ? "" : "  ") + format("%.3f V", volts);
    return text.empty() ? "N/A" : text;
}

std::vector<OverlayRow> overlayRows(const OverlayNumbers& numbers) {
    const bool fahrenheit = numbers.fahrenheit != 0;
    return {
        {"CPU", true, numbers.cpuPercent, 0.50f, 0.80f, overlaySensorText(numbers.cpuCelsius, numbers.cpuVolts, fahrenheit), true},
        {"RAM", numbers.ramTotalGb > 0.0f, numbers.ramPercent, 0.65f, 0.85f,
         format("%.1f / %.1f GB", numbers.ramUsedGb, numbers.ramTotalGb), false},
        {"GPU", numbers.gpuHasLoad != 0, numbers.gpuPercent, 0.50f, 0.85f,
         overlaySensorText(numbers.gpuCelsius, numbers.gpuVolts, fahrenheit), true},
    };
}

// JARV: see-through glass with corner brackets, an LED meter and glowing numbers.
// Kept faint so it doesn't pull the eye from the game; a dark shadow under the
// text keeps it readable over bright scenes.
ImVec2 drawJarvOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 11.0f, kValueSize = 22.0f, kDetailSize = 17.0f;
    constexpr float kPaddingX = 12.0f, kPaddingY = 7.0f, kRowHeight = 24.0f, kColumnGap = 10.0f;
    constexpr float kMeterWidth = 64.0f, kMeterHeight = 7.0f;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.jarvDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.jarvText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.jarvText, kValueSize, "100%").x;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kMeterWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    // Glass, a faint border, and bright corner brackets (as on the JARV panels).
    draw->AddRectFilledMultiColor(ImVec2(0, 0), size, withAlpha(kJarvBgTop, 0.42f), withAlpha(kJarvBgTop, 0.42f),
                                  withAlpha(kJarvBgTop, 0.30f), withAlpha(kJarvBgTop, 0.30f));
    draw->AddRect(ImVec2(0.5f, 0.5f), size - ImVec2(0.5f, 0.5f), withAlpha(kJarvCyan, 0.16f));
    constexpr float kBracket = 8.0f;
    const ImVec2 corners[] = {ImVec2(1, 1), ImVec2(size.x - 1, 1), size - ImVec2(1, 1), ImVec2(1, size.y - 1)};
    const ImVec2 inwards[] = {ImVec2(1, 1), ImVec2(-1, 1), ImVec2(-1, -1), ImVec2(1, -1)};
    for (int i = 0; i < 4; ++i) {
        draw->PathLineTo(ImVec2(corners[i].x + inwards[i].x * kBracket, corners[i].y));
        draw->PathLineTo(corners[i]);
        draw->PathLineTo(ImVec2(corners[i].x, corners[i].y + inwards[i].y * kBracket));
        draw->PathStroke(withAlpha(kJarvCyan, 0.75f), 1.5f);
    }

    const auto shadowedText = [&](ImFont* font, float fontSize, ImVec2 pos, ImU32 color, const std::string& text) {
        draw->AddText(font, fontSize, pos + ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 150), text.c_str());
        draw->AddText(font, fontSize, pos, color, text.c_str());
    };

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = rowTop + kRowHeight * 0.5f;
        float x = kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.jarvDisplay, kLabelSize, row.label);
        shadowedText(fonts.jarvDisplay, kLabelSize, ImVec2(x, midY - labelExtent.y * 0.5f), withAlpha(kJarvCyan, 0.95f), row.label);
        x += labelWidth + kColumnGap;

        const float meterTop = std::floor(midY - kMeterHeight * 0.5f);
        drawSegmentMeter(draw, Box{ImVec2(x, meterTop), ImVec2(x + kMeterWidth, meterTop + kMeterHeight)},
                         row.hasPercent ? row.percent : 0.0f, row.warnAt, row.critAt);
        x += kMeterWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec4& valueColor = row.hasPercent ? jarvLoadColor(row.percent, row.warnAt, row.critAt) : kJarvMuted;
        const ImVec2 valueExtent = textSize(fonts.jarvText, kValueSize, value);
        shadowedText(fonts.jarvText, kValueSize, ImVec2(x + valueWidth - valueExtent.x, midY - valueExtent.y * 0.5f),
                     withAlpha(valueColor, 1.0f), value);
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.jarvText, kDetailSize, row.detail);
        shadowedText(fonts.jarvText, kDetailSize, ImVec2(x, midY - detailExtent.y * 0.5f), withAlpha(kJarvIce, 0.85f), row.detail);
        rowTop += kRowHeight;
    }
    return size;
}

// Galactic Conflict: a notched dark panel with blue line work. Each row has a
// comb of ticks that light up to the value.
ImVec2 drawGalacticOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 10.0f, kValueSize = 15.0f, kDetailSize = 16.0f;
    constexpr float kPaddingX = 16.0f, kPaddingY = 8.0f, kRowHeight = 24.0f, kColumnGap = 10.0f;
    constexpr int kTicks = 20;
    constexpr float kTickStep = 3.0f, kCombWidth = kTicks * kTickStep;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.galacticDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.galacticText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.galacticDisplay, kValueSize, "100").x + textSize(fonts.galacticText, kValueSize, "%").x + 2.0f;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kCombWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    const std::array<ImVec2, 6> outline = notchedCorners(Box{ImVec2(0.5f, 0.5f), size - ImVec2(0.5f, 0.5f)}, 8.0f);
    draw->AddConvexPolyFilled(outline.data(), static_cast<int>(outline.size()), withAlpha(kGcPanel, 0.50f));
    draw->AddPolyline(outline.data(), static_cast<int>(outline.size()), withAlpha(kGcBlue, 0.60f), ImDrawFlags_Closed, 1.0f);
    draw->AddRectFilled(ImVec2(size.x - 14.0f, 4.0f), ImVec2(size.x - 6.0f, 6.0f), withAlpha(kGcRed, 0.9f));

    const auto shadowedText = [&](ImFont* font, float fontSize, ImVec2 pos, ImU32 color, const std::string& text) {
        draw->AddText(font, fontSize, pos + ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 160), text.c_str());
        draw->AddText(font, fontSize, pos, color, text.c_str());
    };

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = rowTop + kRowHeight * 0.5f;
        float x = kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.galacticDisplay, kLabelSize, row.label);
        shadowedText(fonts.galacticDisplay, kLabelSize, ImVec2(x, std::floor(midY - labelExtent.y * 0.5f)), withAlpha(kGcSky, 1.0f), row.label);
        x += labelWidth + kColumnGap;

        const ImVec4& color = galacticLoadColor(row.percent, row.warnAt, row.critAt);
        const float lit = row.hasPercent ? std::clamp(row.percent, 0.0f, 100.0f) / 100.0f * kTicks : 0.0f;
        for (int i = 0; i < kTicks; ++i) {
            const float tickX = std::floor(x + kTickStep * static_cast<float>(i)) + 0.5f;
            const bool on = static_cast<float>(i) < lit;
            draw->AddLine(ImVec2(tickX, midY - (on ? 5.0f : 4.0f)), ImVec2(tickX, midY + (on ? 5.0f : 4.0f)),
                          on ? withAlpha(color, 1.0f) : withAlpha(kGcBlue, 0.30f), on ? 1.5f : 1.0f);
        }
        x += kCombWidth + kColumnGap;

        // The number in Michroma, its "%" in Saira (Michroma's looks like "o/o").
        const ImVec4& valueColor = !row.hasPercent ? kGcSky : (row.percent / 100.0f > row.warnAt) ? color : kGcWhite;
        const std::string number = row.hasPercent ? format("%.0f", row.percent) : "N/A";
        const ImVec2 numberExtent = textSize(fonts.galacticDisplay, kValueSize, number);
        const float signWidth = row.hasPercent ? textSize(fonts.galacticText, kValueSize, "%").x + 2.0f : 0.0f;
        const float numberLeft = x + valueWidth - signWidth - numberExtent.x;
        shadowedText(fonts.galacticDisplay, kValueSize, ImVec2(numberLeft, std::floor(midY - numberExtent.y * 0.5f)), withAlpha(valueColor, 1.0f),
                     number);
        if (row.hasPercent) {
            const ImVec2 signExtent = textSize(fonts.galacticText, kValueSize, "%");
            shadowedText(fonts.galacticText, kValueSize, ImVec2(numberLeft + numberExtent.x + 2.0f, std::floor(midY - signExtent.y * 0.5f)),
                         withAlpha(valueColor, 1.0f), "%");
        }
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.galacticText, kDetailSize, row.detail);
        shadowedText(fonts.galacticText, kDetailSize, ImVec2(x, std::floor(midY - detailExtent.y * 0.5f)), withAlpha(kGcSky, 1.0f), row.detail);
        rowTop += kRowHeight;
    }
    return size;
}

// Federation Gunship: a see-through dialogue box in outlined pixel text, with
// five energy tanks per row (one per 20%).
ImVec2 drawGunshipOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kPaddingX = 14.0f, kPaddingY = 10.0f, kRowHeight = 22.0f, kColumnGap = 10.0f;
    constexpr int kTanks = 5;
    constexpr float kTank = 8.0f, kTankGap = 4.0f, kTanksWidth = kTanks * kTank + (kTanks - 1) * kTankGap;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.gunshipDisplay, 8.0f, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.gunshipText, 16.0f, row.detail).x);
    }
    const float valueWidth = textSize(fonts.gunshipDisplay, 16.0f, "100%").x;
    const ImVec2 size = snapToPixel(ImVec2(kPaddingX * 2.0f + labelWidth + kTanksWidth + valueWidth + detailWidth + kColumnGap * 3.0f + kPx,
                                           kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size()) + kPx));
    drawPixelBox(draw, Box{ImVec2(0.0f, 0.0f), size}, 0.62f);

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = snapToPixel(rowTop + kRowHeight * 0.5f);
        float x = kPaddingX;
        drawOutlinedText(draw, fonts.gunshipDisplay, 8.0f, ImVec2(x, midY - 4.0f), kFgYellow, row.label);
        x += labelWidth + kColumnGap;

        const ImVec4& color = gunshipLoadColor(row.percent, row.warnAt, row.critAt);
        const int lit = row.hasPercent ? static_cast<int>(std::lround(std::clamp(row.percent, 0.0f, 100.0f) / 20.0f)) : 0;
        drawEnergyTanks(draw, ImVec2(x, midY - kTank * 0.5f), kTanks, lit, kTank, kTankGap, color);
        x += kTanksWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const float valueLeft = x + valueWidth - textSize(fonts.gunshipDisplay, 16.0f, value).x;
        drawOutlinedText(draw, fonts.gunshipDisplay, 16.0f, ImVec2(valueLeft, midY - 8.0f), row.hasPercent ? kFgWhite : kFgGray, value, kPx);
        x += valueWidth + kColumnGap;

        drawOutlinedText(draw, fonts.gunshipText, 16.0f, ImVec2(x, midY - 9.0f), kFgWhite, row.detail);
        rowTop += kRowHeight;
    }
    return size;
}

// Halloween: a see-through stone slab with a cobweb in the corner; each row
// has a chunky goo bar.
ImVec2 drawHalloweenOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 20.0f, kValueSize = 26.0f, kDetailSize = 15.0f;   // Henny Penny runs small (see above)
    constexpr float kPaddingX = 16.0f, kPaddingY = 8.0f, kRowHeight = 27.0f, kColumnGap = 10.0f;
    constexpr float kBarWidth = 64.0f, kBarHeight = 10.0f;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.halloweenDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.halloweenText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.halloweenDisplay, kValueSize, "100%").x;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kBarWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    // Inset by the slab's outline and shadow so they stay inside the window.
    const Box slab{ImVec2(4.0f, 4.0f), size + ImVec2(4.0f, 4.0f)};
    drawStoneSlab(draw, slab, 12.0f, 0.55f);
    drawCobweb(draw, ImVec2(slab.max.x - 3.0f, slab.min.y + 3.0f), 24.0f, -1.0f, 1.0f, 0.45f);

    float rowTop = slab.min.y + kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = std::floor(rowTop + kRowHeight * 0.5f);
        float x = slab.min.x + kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.halloweenDisplay, kLabelSize, row.label);
        drawHalloweenText(draw, fonts.halloweenDisplay, kLabelSize, ImVec2(x, midY - labelExtent.y * 0.5f), kHwOrange, row.label, 1.5f);
        x += labelWidth + kColumnGap;

        // A chunky bar: outlined dark track, the value in goo, a glossy line along the top.
        const ImVec2 barA(x, midY - kBarHeight * 0.5f), barB(x + kBarWidth, midY + kBarHeight * 0.5f);
        draw->AddRectFilled(barA - ImVec2(2.0f, 2.0f), barB + ImVec2(2.0f, 2.0f), withAlpha(kHwOutline, 1.0f), kBarHeight * 0.5f + 2.0f);
        draw->AddRectFilled(barA, barB, withAlpha(kHwPlaque, 1.0f), kBarHeight * 0.5f);
        if (row.hasPercent && row.percent > 0.5f) {
            const float fillRight = barA.x + std::max(kBarHeight, kBarWidth * std::clamp(row.percent, 0.0f, 100.0f) / 100.0f);
            draw->AddRectFilled(barA, ImVec2(fillRight, barB.y), withAlpha(halloweenLoadColor(row.percent, row.warnAt, row.critAt), 1.0f), kBarHeight * 0.5f);
            draw->AddLine(ImVec2(barA.x + 4.0f, barA.y + 2.5f), ImVec2(fillRight - 4.0f, barA.y + 2.5f), withAlpha(kHwBone, 0.45f), 1.5f);
        }
        x += kBarWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec2 valueExtent = textSize(fonts.halloweenDisplay, kValueSize, value);
        drawHalloweenText(draw, fonts.halloweenDisplay, kValueSize, ImVec2(x + valueWidth - valueExtent.x, midY - valueExtent.y * 0.5f),
                          row.hasPercent ? kHwBone : kHwMist, value);
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.halloweenText, kDetailSize, row.detail);
        drawHalloweenText(draw, fonts.halloweenText, kDetailSize, ImVec2(x, midY - detailExtent.y * 0.5f), kHwBone, row.detail, 1.0f);
        rowTop += kRowHeight;
    }
    return slab.max + ImVec2(6.0f, 8.0f);   // past the slab's outline and shadow
}

// Classic: looks like a small ImGui window from the main window's sections:
// the same heading colours and green / orange / red usage bars. It is drawn
// with shapes in ImGui's own Classic style colours rather than with widgets,
// because ImGui won't size a window larger than the one it is drawing in, and
// the overlay window has to be fitted to the panel.
ImVec2 drawClassicOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    const ImVec4 labelColors[] = {kCpuHeading, kRamHeading, kGpuHeading};
    ImFont* font = fonts.classic;
    constexpr float kFontSize = 13.0f, kBarWidth = 120.0f;   // ProggyClean's size; the bar as in the old widget version

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(font, kFontSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(font, kFontSize, row.detail).x);
    }
    const float frameHeight = kFontSize + style.FramePadding.y * 2.0f;
    const float rowHeight = frameHeight + style.CellPadding.y * 2.0f;
    const float columnGap = style.CellPadding.x * 2.0f;
    const ImVec2 size(std::ceil(style.WindowPadding.x * 2.0f + labelWidth + columnGap + kBarWidth + columnGap + detailWidth),
                      std::ceil(style.WindowPadding.y * 2.0f + rowHeight * static_cast<float>(rows.size())));

    ImVec4 background = style.Colors[ImGuiCol_WindowBg];
    background.w = 0.80f;
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, ImGui::ColorConvertFloat4ToU32(background));
    draw->AddRect(ImVec2(0.0f, 0.0f), size, ImGui::GetColorU32(ImGuiCol_Border));

    float top = style.WindowPadding.y + style.CellPadding.y;
    for (size_t i = 0; i < rows.size(); ++i) {
        const OverlayRow& row = rows[i];
        const float textY = top + style.FramePadding.y;
        float x = style.WindowPadding.x;
        draw->AddText(font, kFontSize, ImVec2(x, textY), withAlpha(labelColors[i], 1.0f), row.label);
        x += labelWidth + columnGap;

        // A ProgressBar: frame, fill, and the text just past the end of the fill.
        const ImVec2 barMin(x, top), barMax(x + kBarWidth, top + frameHeight);
        draw->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        const float fraction = row.hasPercent ? std::clamp(row.percent / 100.0f, 0.0f, 1.0f) : 0.0f;
        const float fillRight = barMin.x + kBarWidth * fraction;
        if (fraction > 0.0f) {
            draw->AddRectFilled(barMin, ImVec2(fillRight, barMax.y), withAlpha(usageColor(row.percent, row.warnAt, row.critAt), 1.0f),
                                style.FrameRounding);
        }
        const std::string value = row.hasPercent ? format("%.1f%%", row.percent) : "N/A";
        const float valueWidth = textSize(font, kFontSize, value).x;
        const float valueX = std::clamp(fillRight + style.ItemSpacing.x, barMin.x, barMax.x - valueWidth - style.ItemInnerSpacing.x);
        draw->AddText(font, kFontSize, ImVec2(std::floor(valueX), textY), ImGui::GetColorU32(ImGuiCol_Text), value.c_str());
        x += kBarWidth + columnGap;

        const ImU32 detailColor = row.detail == "N/A"  ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                                  : row.detailIsVoltage ? withAlpha(kVoltageColor, 1.0f)
                                                        : ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddText(font, kFontSize, ImVec2(x, textY), detailColor, row.detail.c_str());
        top += rowHeight;
    }
    return size;
}

// Poseidon: dark sea glass with a gold border and a Greek key band along the
// top; a water-filled tube per row and the number in carved capitals.
ImVec2 drawPoseidonOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 11.0f, kValueSize = 18.0f, kDetailSize = 16.0f;
    constexpr float kPaddingX = 12.0f, kPaddingTop = 19.0f, kPaddingBottom = 6.0f, kRowHeight = 24.0f, kColumnGap = 10.0f;
    constexpr float kTubeWidth = 60.0f, kTubeHeight = 8.0f;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.poseidonDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.poseidonText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.poseidonDisplay, kValueSize, "100%").x;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kTubeWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingTop + kPaddingBottom + kRowHeight * static_cast<float>(rows.size())));

    draw->AddRectFilled(ImVec2(0, 0), size, withAlpha(kPoPanel, 0.62f), 3.0f);
    draw->AddRect(ImVec2(0.5f, 0.5f), size - ImVec2(0.5f, 0.5f), withAlpha(kPoGold, 0.80f), 3.0f);
    drawGreekKey(draw, 6.0f, size.x - 6.0f, 4.0f, 8.0f, withAlpha(kPoGold, 0.55f));

    const auto shadowedText = [&](ImFont* font, float fontSize, ImVec2 pos, ImU32 color, const std::string& text) {
        draw->AddText(font, fontSize, pos + ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 160), text.c_str());
        draw->AddText(font, fontSize, pos, color, text.c_str());
    };

    float rowTop = kPaddingTop;
    for (const OverlayRow& row : rows) {
        const float midY = rowTop + kRowHeight * 0.5f;
        float x = kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.poseidonDisplay, kLabelSize, row.label);
        shadowedText(fonts.poseidonDisplay, kLabelSize, ImVec2(x, std::floor(midY - labelExtent.y * 0.5f)), withAlpha(kPoGold, 1.0f), row.label);
        x += labelWidth + kColumnGap;

        const ImVec4& color = poseidonLoadColor(row.percent, row.warnAt, row.critAt);
        const Box tube{ImVec2(x, std::floor(midY - kTubeHeight * 0.5f)), ImVec2(x + kTubeWidth, std::floor(midY + kTubeHeight * 0.5f))};
        draw->AddRectFilled(tube.min, tube.max, withAlpha(kPoSea, 0.15f), 4.0f);
        if (row.hasPercent && row.percent > 0.5f) {
            draw->AddRectFilled(tube.min, ImVec2(tube.min.x + std::max(kTubeWidth * std::min(row.percent, 100.0f) / 100.0f, 8.0f), tube.max.y),
                                withAlpha(color, 1.0f), 4.0f);
        }
        draw->AddRect(tube.min, tube.max, withAlpha(kPoGold, 0.55f), 4.0f);
        x += kTubeWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec2 valueExtent = textSize(fonts.poseidonDisplay, kValueSize, value);
        shadowedText(fonts.poseidonDisplay, kValueSize, ImVec2(x + valueWidth - valueExtent.x, std::floor(midY - valueExtent.y * 0.5f)),
                     withAlpha(!row.hasPercent ? kPoFoam : (row.percent / 100.0f > row.warnAt) ? color : kPoMarble, 1.0f), value);
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.poseidonText, kDetailSize, row.detail);
        shadowedText(fonts.poseidonText, kDetailSize, ImVec2(x, std::floor(midY - detailExtent.y * 0.5f)), withAlpha(kPoFoam, 1.0f), row.detail);
        rowTop += kRowHeight;
    }
    return size;
}

// Otaku: a rounded navy card with a teal edge; violet name chips, candy pill
// bars and big outlined numbers.
ImVec2 drawOtakuOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kChipWidth = 38.0f, kValueSize = 17.0f, kDetailSize = 14.0f;
    constexpr float kPaddingX = 12.0f, kPaddingY = 8.0f, kRowHeight = 25.0f, kColumnGap = 9.0f, kBarWidth = 58.0f;

    float detailWidth = 0.0f;
    for (const OverlayRow& row : rows) detailWidth = std::max(detailWidth, textSize(fonts.otakuText, kDetailSize, row.detail).x);
    const float valueWidth = textSize(fonts.otakuDisplay, kValueSize, "100%").x + 4.0f;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + kChipWidth + kBarWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    draw->AddRectFilled(ImVec2(0, 0), size, withAlpha(kOtNavy, 0.72f), 12.0f);
    draw->AddRect(ImVec2(1.0f, 1.0f), size - ImVec2(1.0f, 1.0f), withAlpha(kOtTeal, 0.90f), 12.0f, 0, 2.0f);
    drawSparkle(draw, ImVec2(size.x - 10.0f, 9.0f), 4.0f, withAlpha(kOtLemon, 1.0f));

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = std::floor(rowTop + kRowHeight * 0.5f);
        float x = kPaddingX;
        draw->AddRectFilled(ImVec2(x, midY - 8.0f), ImVec2(x + kChipWidth, midY + 8.0f), withAlpha(kOtViolet, 0.95f), 8.0f);
        drawCenteredText(draw, fonts.otakuText, 12.0f, ImVec2(x + kChipWidth * 0.5f, midY), withAlpha(kOtWhite, 1.0f), row.label);
        x += kChipWidth + kColumnGap;

        drawPillBar(draw, Box{ImVec2(x, midY - 5.0f), ImVec2(x + kBarWidth, midY + 5.0f)}, row.hasPercent ? row.percent : 0.0f,
                    otakuLoadColor(row.percent, row.warnAt, row.critAt));
        x += kBarWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec2 valueExtent = textSize(fonts.otakuDisplay, kValueSize, value);
        drawInkText(draw, fonts.otakuDisplay, kValueSize, ImVec2(x + valueWidth - valueExtent.x, std::floor(midY - valueExtent.y * 0.5f)),
                    withAlpha(kOtWhite, 1.0f), value, 1.5f);
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.otakuText, kDetailSize, row.detail);
        drawInkText(draw, fonts.otakuText, kDetailSize, ImVec2(x, std::floor(midY - detailExtent.y * 0.5f)), withAlpha(kOtLilac, 1.0f), row.detail, 1.0f);
        rowTop += kRowHeight;
    }
    return size;
}

// Cyber-Punk: a near-black panel with cut corners and a red outline; slanted
// segment bars and cyan numbers.
ImVec2 drawCyberpunkOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 11.0f, kValueSize = 18.0f, kDetailSize = 17.0f;
    constexpr float kPaddingX = 14.0f, kPaddingY = 7.0f, kRowHeight = 24.0f, kColumnGap = 10.0f, kBarWidth = 70.0f;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.cyberpunkDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.cyberpunkText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.cyberpunkDisplay, kValueSize, "100%").x;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kBarWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    const std::array<ImVec2, 6> outline = cutCorners(Box{ImVec2(0.5f, 0.5f), size - ImVec2(0.5f, 0.5f)}, 10.0f);
    draw->AddConvexPolyFilled(outline.data(), 6, withAlpha(kCpPanel, 0.62f));
    draw->AddPolyline(outline.data(), 6, withAlpha(kCpRed, 0.75f), ImDrawFlags_Closed, 1.0f);
    draw->AddRectFilled(ImVec2(0.0f, 4.0f), ImVec2(3.0f, 26.0f), withAlpha(kCpRed, 1.0f));

    const auto shadowedText = [&](ImFont* font, float fontSize, ImVec2 pos, ImU32 color, const std::string& text) {
        draw->AddText(font, fontSize, pos + ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 170), text.c_str());
        draw->AddText(font, fontSize, pos, color, text.c_str());
    };

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = std::floor(rowTop + kRowHeight * 0.5f);
        float x = kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.cyberpunkDisplay, kLabelSize, row.label);
        shadowedText(fonts.cyberpunkDisplay, kLabelSize, ImVec2(x, std::floor(midY - labelExtent.y * 0.5f)), withAlpha(kCpRed, 1.0f), row.label);
        x += labelWidth + kColumnGap;

        const ImVec4& color = cyberpunkLoadColor(row.percent, row.warnAt, row.critAt);
        drawSlantedSegments(draw, Box{ImVec2(x, midY - 4.0f), ImVec2(x + kBarWidth, midY + 4.0f)}, row.hasPercent ? row.percent : 0.0f, color);
        x += kBarWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec2 valueExtent = textSize(fonts.cyberpunkDisplay, kValueSize, value);
        shadowedText(fonts.cyberpunkDisplay, kValueSize, ImVec2(x + valueWidth - valueExtent.x, std::floor(midY - valueExtent.y * 0.5f)),
                     withAlpha(row.hasPercent ? color : kCpDimRed, 1.0f), value);
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.cyberpunkText, kDetailSize, row.detail);
        shadowedText(fonts.cyberpunkText, kDetailSize, ImVec2(x, std::floor(midY - detailExtent.y * 0.5f)), withAlpha(kCpText, 1.0f), row.detail);
        rowTop += kRowHeight;
    }
    return size;
}

// Classroom: like GNOME's own on-screen pop-ups (volume, brightness): a dark
// see-through rounded panel, white text and thin level bars.
ImVec2 drawGnomeOverlay(ImDrawList* draw, const OverlayNumbers& numbers, const UiFonts& fonts) {
    const std::vector<OverlayRow> rows = overlayRows(numbers);
    constexpr float kLabelSize = 14.0f, kValueSize = 16.0f, kDetailSize = 14.0f;
    constexpr float kPaddingX = 16.0f, kPaddingY = 9.0f, kRowHeight = 24.0f, kColumnGap = 12.0f, kBarWidth = 72.0f;

    float labelWidth = 0.0f, detailWidth = 0.0f;
    for (const OverlayRow& row : rows) {
        labelWidth = std::max(labelWidth, textSize(fonts.gnomeDisplay, kLabelSize, row.label).x);
        detailWidth = std::max(detailWidth, textSize(fonts.gnomeText, kDetailSize, row.detail).x);
    }
    const float valueWidth = textSize(fonts.gnomeDisplay, kValueSize, "100%").x;
    const ImVec2 size(std::ceil(kPaddingX * 2.0f + labelWidth + kBarWidth + valueWidth + detailWidth + kColumnGap * 3.0f),
                      std::ceil(kPaddingY * 2.0f + kRowHeight * static_cast<float>(rows.size())));

    const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(28, 28, 32, 190), 14.0f);
    draw->AddRect(ImVec2(0.5f, 0.5f), size - ImVec2(0.5f, 0.5f), IM_COL32(255, 255, 255, 30), 14.0f);

    float rowTop = kPaddingY;
    for (const OverlayRow& row : rows) {
        const float midY = std::floor(rowTop + kRowHeight * 0.5f);
        float x = kPaddingX;
        const ImVec2 labelExtent = textSize(fonts.gnomeDisplay, kLabelSize, row.label);
        draw->AddText(fonts.gnomeDisplay, kLabelSize, ImVec2(x, std::floor(midY - labelExtent.y * 0.5f)), withAlpha(white, 1.0f), row.label);
        x += labelWidth + kColumnGap;

        // Bars are white, as in GNOME's pop-ups, until the load gets high.
        const float fraction = row.percent / 100.0f;
        const ImVec4& barColor = !row.hasPercent ? white : fraction > row.critAt ? kGnomeRed : fraction > row.warnAt ? kGnomeYellow : white;
        const Box bar{ImVec2(x, midY - 3.0f), ImVec2(x + kBarWidth, midY + 3.0f)};
        draw->AddRectFilled(bar.min, bar.max, IM_COL32(255, 255, 255, 50), 3.0f);
        if (row.hasPercent && row.percent > 0.5f) {
            draw->AddRectFilled(bar.min, ImVec2(bar.min.x + std::max(kBarWidth * std::min(row.percent, 100.0f) / 100.0f, 6.0f), bar.max.y),
                                withAlpha(barColor, 1.0f), 3.0f);
        }
        x += kBarWidth + kColumnGap;

        const std::string value = row.hasPercent ? format("%.0f%%", row.percent) : "N/A";
        const ImVec2 valueExtent = textSize(fonts.gnomeDisplay, kValueSize, value);
        draw->AddText(fonts.gnomeDisplay, kValueSize, ImVec2(x + valueWidth - valueExtent.x, std::floor(midY - valueExtent.y * 0.5f)),
                      withAlpha(white, 1.0f), value.c_str());
        x += valueWidth + kColumnGap;

        const ImVec2 detailExtent = textSize(fonts.gnomeText, kDetailSize, row.detail);
        draw->AddText(fonts.gnomeText, kDetailSize, ImVec2(x, std::floor(midY - detailExtent.y * 0.5f)), withAlpha(white, 0.75f), row.detail.c_str());
        rowTop += kRowHeight;
    }
    return size;
}

// `CpuMonitor --overlay`: the overlay window. Takes orders from the main process
// on stdin ("stats ...", "skin <id>" (see kSkinChoices), "toggle app" / "toggle
// screen"), reports hotkey presses ("hotkey") and whether it is on screen
// ("active 1" / "active 0") on stdout, and quits when the main process closes the pipe.
int runOverlayProcess() {
    prctl(PR_SET_PDEATHSIG, SIGTERM);   // never outlive the main process

    static X11Api x11;   // static: the hotkey thread uses it until the process ends
    if (!x11.load()) return 1;
    x11.initThreads();   // two threads use X11; older libX11 versions need to be told
    x11.setErrorHandler(ignoreX11Error);

    glfwSetErrorCallback(onGlfwError);
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);   // clicks go to the game underneath
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "veeastats");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "veeastats-overlay");
    // Any size: the first frame fits the window to the panel (see drawFrame).
    GLFWwindow* window = glfwCreateWindow(240, 90, "VeeaStats Overlay", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    // Override-redirect: the window manager leaves the window alone, so it stays
    // on top, never takes focus and goes exactly where it is put.
    Display* display = glfwGetX11Display();
    const Window overlayWindow = glfwGetX11Window(window);
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    x11.changeWindowAttributes(display, overlayWindow, CWOverrideRedirect, &attributes);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, false);   // the overlay takes no input
#if defined(IMGUI_IMPL_OPENGL_ES3)
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 130");
#endif
    const UiFonts fonts = loadFonts();

    std::thread(watchX11Hotkey, &x11).detach();
    setNonBlocking(STDIN_FILENO);

    OverlayNumbers numbers;
    Skin skin = Skin::Jarv;
    const auto useSkin = [&](Skin newSkin) {
        skin = newSkin;
        applySkin(skin, fonts);   // the Classic look takes its colours and spacing from ImGui's style
    };
    useSkin(Skin::Jarv);

    // The window is fitted to the panel after every frame, so nothing but the
    // panel is ever on screen. Without a compositor (some X11 desktops, or KDE's
    // X11 session while a fullscreen game turns compositing off), the see-through
    // parts of a window show up black.
    int windowWidth = 240, windowHeight = 90;
    const auto drawFrame = [&] {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        ImVec2 panel;
        switch (skin) {
            case Skin::Classic:   panel = drawClassicOverlay(draw, numbers, fonts);   break;
            case Skin::Jarv:      panel = drawJarvOverlay(draw, numbers, fonts);      break;
            case Skin::Galactic:  panel = drawGalacticOverlay(draw, numbers, fonts);  break;
            case Skin::Gunship:   panel = drawGunshipOverlay(draw, numbers, fonts);   break;
            case Skin::Halloween: panel = drawHalloweenOverlay(draw, numbers, fonts); break;
            case Skin::Poseidon:  panel = drawPoseidonOverlay(draw, numbers, fonts);  break;
            case Skin::Otaku:     panel = drawOtakuOverlay(draw, numbers, fonts);     break;
            case Skin::Cyberpunk: panel = drawCyberpunkOverlay(draw, numbers, fonts); break;
            case Skin::Gnome:     panel = drawGnomeOverlay(draw, numbers, fonts);     break;
        }
        ImGui::Render();

        int framebufferWidth, framebufferHeight;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        glViewport(0, 0, framebufferWidth, framebufferHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        const int width = static_cast<int>(std::ceil(panel.x)), height = static_cast<int>(std::ceil(panel.y));
        if (width != windowWidth || height != windowHeight) {
            windowWidth = width;
            windowHeight = height;
            glfwSetWindowSize(window, width, height);
        }
    };

    // What the overlay was opened on:
    //   Window: the focused app ("Limit Overlay to Focused App" on). It shows only
    //           while that app has focus, and closes for good when the app does.
    //           Only X11 apps can be followed (Proton/Wine games are X11).
    //   Screen: the main screen's corner (the setting off). It stays there,
    //           whatever has focus, until the hotkey closes it.
    enum class OpenedOn { Nothing, Window, Screen };
    OpenedOn openedOn = OpenedOn::Nothing;
    Window target = None;
    bool mapped = false;   // actually on screen right now

    const auto report = [](bool active) {
        if (write(STDOUT_FILENO, active ? "active 1\n" : "active 0\n", 9) < 0) {
            // the main process is gone; reading stdin will notice and end the loop
        }
    };
    const auto showAt = [&](ImVec2 corner) {
        x11.moveWindow(display, overlayWindow, static_cast<int>(corner.x + kOverlayMargin),
                       static_cast<int>(corner.y + kOverlayMargin));
        if (!mapped) drawFrame();   // the first frame fits the window to the panel before it appears
        drawFrame();
        if (!mapped) {
            glfwShowWindow(window);
            mapped = true;
            report(true);
        }
    };
    const auto hideFromScreen = [&] {
        if (!mapped) return;
        glfwHideWindow(window);
        mapped = false;
        report(false);
    };
    const auto closeOverlay = [&] {
        openedOn = OpenedOn::Nothing;
        target = None;
        hideFromScreen();
    };
    // The hotkey closes the overlay if it's on screen. Otherwise (closed, or
    // hidden because its app isn't focused) it opens it: on the focused app when
    // `limitToApp`, else in the screen's corner. With `limitToApp` and no app it
    // can follow in focus (a Wayland window, say), the press does nothing.
    const auto toggle = [&](bool limitToApp) {
        if (mapped) {
            closeOverlay();
            return;
        }
        if (!limitToApp) {
            target = None;
            openedOn = OpenedOn::Screen;
        } else {
            const Window focused = focusedX11Window(x11, display, overlayWindow);
            if (focused == None) return;
            target = focused;
            openedOn = OpenedOn::Window;
        }
    };

    std::string input;
    while (true) {
        // While open, wake 4 times a second to follow (or notice the end of) its window.
        pollfd waitForInput{STDIN_FILENO, POLLIN, 0};
        poll(&waitForInput, 1, openedOn != OpenedOn::Nothing ? 250 : -1);

        char chunk[512];
        ssize_t bytes;
        while ((bytes = read(STDIN_FILENO, chunk, sizeof(chunk))) > 0) input.append(chunk, static_cast<size_t>(bytes));
        if (bytes == 0) break;   // the main process has closed
        size_t newline;
        while ((newline = input.find('\n')) != std::string::npos) {
            const std::string line = input.substr(0, newline);
            input.erase(0, newline + 1);
            if (line == "toggle app")         toggle(true);
            else if (line == "toggle screen") toggle(false);
            else if (startsWith(line, "skin ")) {
                for (const SkinChoice& choice : kSkinChoices) {
                    if (line.substr(strlen("skin ")) == choice.id) useSkin(choice.skin);
                }
            } else {
                parseOverlayStatsLine(line, numbers);
            }
        }
        glfwPollEvents();

        if (openedOn == OpenedOn::Screen) {
            showAt(screenCorner(x11, display));
        } else if (openedOn == OpenedOn::Window) {
            const std::optional<ImVec2> corner = windowCorner(x11, display, target);
            if (!corner) {
                closeOverlay();     // its app has closed
            } else if (focusedX11Window(x11, display, overlayWindow) == target) {
                showAt(*corner);
            } else {
                hideFromScreen();   // until its app is focused again
            }
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwTerminate();
    return 0;
}

// The overlay process, seen from the main program.
class OverlayProcess {
public:
    OverlayProcess() = default;
    OverlayProcess(const OverlayProcess&) = delete;
    OverlayProcess& operator=(const OverlayProcess&) = delete;
    ~OverlayProcess() { stop(); }

    bool active() const { return pid_ > 0 && g_overlayActivePid == pid_; }

    // Starts `CpuMonitor --overlay`, if there is an X11 display (XWayland
    // counts) to show it on.
    void start() {
        if (!x11DisplayAvailable()) return;

        int toOverlay[2], fromOverlay[2];
        if (pipe2(toOverlay, O_CLOEXEC) != 0) return;
        if (pipe2(fromOverlay, O_CLOEXEC) != 0) {
            close(toOverlay[0]);
            close(toOverlay[1]);
            return;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, toOverlay[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fromOverlay[1], STDOUT_FILENO);
        const char* const args[] = {"CpuMonitor", "--overlay", nullptr};
        const int result = posix_spawn(&pid_, "/proc/self/exe", &actions, nullptr, const_cast<char* const*>(args), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(toOverlay[0]);
        close(fromOverlay[1]);
        if (result != 0) {
            close(toOverlay[1]);
            close(fromOverlay[0]);
            pid_ = -1;
            return;
        }
        fd_ = toOverlay[1];
        setNonBlocking(fd_);   // a stuck overlay must never freeze the main window
        std::thread(readReports, fromOverlay[0], pid_).detach();
        if (!skinLine_.empty()) send(skinLine_);   // a restarted overlay keeps the skin
    }

    // Starts a new overlay if the old one has ended on its own (it crashed, or
    // lost its X display), since on X11 it also carries the hotkey. At most 3
    // restarts a minute, so one that can't start doesn't spin; past that, the
    // restart waits (see secondsUntilRetry). Returns true if it started one.
    bool restartIfEnded() {
        if (!x11DisplayAvailable()) return false;
        // Its reports end a moment before the process itself has, so a report
        // that it ended counts even if waitpid doesn't know yet.
        const pid_t endedPid = g_overlayEndedPid.exchange(-1);
        const bool ended = pid_ > 0 && endedPid == pid_;
        if (fd_ >= 0 && pid_ > 0 && !ended) {
            if (waitpid(pid_, nullptr, WNOHANG) == 0) return false;   // still running
            pid_ = -1;   // reaped just now: its pid may be reused, so never signal it
        }
        stop();   // reaps it, if that's still to do
        retryAt_ = -1.0;
        const double now = glfwGetTime();
        double& oldest = restartTimes_[nextRestart_];
        if (oldest >= 0.0 && now - oldest < 60.0) {
            retryAt_ = oldest + 60.0;
            return false;
        }
        oldest = now;
        nextRestart_ = (nextRestart_ + 1) % restartTimes_.size();
        start();
        return fd_ >= 0;
    }

    // How long until a restart held back by the limit is due (the main loop
    // wakes up for it), or -1 if none is waiting.
    double secondsUntilRetry() const { return retryAt_ < 0.0 ? -1.0 : std::max(retryAt_ - glfwGetTime(), 0.0); }

    // The hotkey was pressed. `stats` is sent first so the overlay never opens
    // with old numbers. `limitToApp` is the "Limit Overlay to Focused App" setting.
    void toggle(const LiveStats& stats, bool limitToApp, bool fahrenheit) {
        // Both hotkey sources can report the same press (X11 sessions where the
        // desktop also has the shortcut), so presses this close together count once.
        constexpr double kTogglePause = 0.3;
        const double now = glfwGetTime();
        if (now - lastToggle_ < kTogglePause) return;
        lastToggle_ = now;

        restartIfEnded();   // the press still opens it, if it had gone
        sendStats(stats, fahrenheit);
        send(limitToApp ? "toggle app\n" : "toggle screen\n");
    }

    void sendStats(const LiveStats& stats, bool fahrenheit) { send(overlayStatsLine(stats, fahrenheit)); }

    // The overlay draws itself in the same skin as the main window.
    void setSkin(Skin skin) {
        skinLine_ = std::string("skin ") + skinChoice(skin).id + "\n";
        send(skinLine_);
    }

private:
    static bool x11DisplayAvailable() {
        const char* display = getenv("DISPLAY");
        return display && *display;
    }

    // Passes the overlay's reports on to the main loop: "hotkey" (from its X11
    // key grab) and "active 1/0". Ends when the overlay process (`pid`) does.
    static void readReports(int fd, pid_t pid) {
        std::string line;
        char c;
        while (read(fd, &c, 1) == 1) {
            if (c != '\n') {
                line += c;
                continue;
            }
            if (line == "hotkey") {
                postRequest(kRequestToggleOverlay);
            } else if (line == "active 1") {
                g_overlayActivePid = pid;
                postRequest(kRequestOverlayChanged);
            } else if (line == "active 0") {
                pid_t expected = pid;   // only if it's still this overlay that counts as on screen
                g_overlayActivePid.compare_exchange_strong(expected, -1);
                postRequest(kRequestOverlayChanged);
            }
            line.clear();
        }
        pid_t expected = pid;
        g_overlayActivePid.compare_exchange_strong(expected, -1);
        close(fd);
        g_overlayEndedPid = pid;
        postRequest(kRequestOverlayEnded);   // the main loop starts a new one (see restartIfEnded)
    }

    // Sends one line. If the overlay has gone away, it's restarted by
    // restartIfEnded, not here.
    void send(const std::string& line) {
        if (fd_ >= 0 && write(fd_, line.data(), line.size()) < 0 && errno != EAGAIN) stop();
    }

    // Ends the overlay. Closing its pipe would end it too, but only at its next
    // look at the pipe, so it's signalled straight away (see endChildProcess).
    void stop() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            endChildProcess(pid_, false);
            pid_ = -1;
        }
    }

    pid_t pid_{-1};
    int fd_{-1};
    double lastToggle_{-1.0};
    std::string skinLine_;                                     // the last "skin ..." line sent
    std::array<double, 3> restartTimes_{-1.0, -1.0, -1.0};      // when the last 3 restarts happened
    size_t nextRestart_{0};
    double retryAt_{-1.0};                                     // when a held-back restart is due, or -1
};

// ---- Hotkey through the desktop's GlobalShortcuts portal ----------------------

// The GLib functions used to talk to the portal over D-Bus.
// G_VARIANT_TYPE() would call into GLib, which isn't linked (it's loaded at
// runtime, see GioApi); a GVariantType is just its type string, so a cast does
// the same.
const GVariantType* variantType(const char* typeString) { return reinterpret_cast<const GVariantType*>(typeString); }

struct GioApi {
    decltype(&g_bus_get_sync) busGetSync{};
    decltype(&g_dbus_connection_get_unique_name) uniqueName{};
    decltype(&g_dbus_connection_call_sync) callSync{};
    decltype(&g_dbus_connection_signal_subscribe) signalSubscribe{};
    decltype(&g_dbus_connection_signal_unsubscribe) signalUnsubscribe{};
    decltype(&g_variant_new_parsed) newParsed{};
    decltype(&g_variant_get) get{};
    decltype(&g_variant_lookup) lookup{};
    decltype(&g_variant_lookup_value) lookupValue{};
    decltype(&g_variant_n_children) childCount{};
    decltype(&g_variant_get_type_string) typeString{};
    decltype(&g_variant_is_object_path) isObjectPath{};
    decltype(&g_variant_unref) unref{};
    decltype(&g_main_context_new) newContext{};
    decltype(&g_main_context_push_thread_default) pushContext{};
    decltype(&g_main_loop_new) newLoop{};
    decltype(&g_main_loop_run) runLoop{};
    decltype(&g_main_loop_quit) quitLoop{};
    decltype(&g_error_free) freeError{};

    bool load() {
        void* library = dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL);   // brings GLib along
        return library && loadSymbol(library, "g_bus_get_sync", busGetSync) &&
               loadSymbol(library, "g_dbus_connection_get_unique_name", uniqueName) &&
               loadSymbol(library, "g_dbus_connection_call_sync", callSync) &&
               loadSymbol(library, "g_dbus_connection_signal_subscribe", signalSubscribe) &&
               loadSymbol(library, "g_dbus_connection_signal_unsubscribe", signalUnsubscribe) &&
               loadSymbol(library, "g_variant_new_parsed", newParsed) && loadSymbol(library, "g_variant_get", get) &&
               loadSymbol(library, "g_variant_lookup", lookup) && loadSymbol(library, "g_variant_lookup_value", lookupValue) &&
               loadSymbol(library, "g_variant_n_children", childCount) && loadSymbol(library, "g_variant_get_type_string", typeString) &&
               loadSymbol(library, "g_variant_is_object_path", isObjectPath) && loadSymbol(library, "g_variant_unref", unref) &&
               loadSymbol(library, "g_main_context_new", newContext) &&
               loadSymbol(library, "g_main_context_push_thread_default", pushContext) &&
               loadSymbol(library, "g_main_loop_new", newLoop) && loadSymbol(library, "g_main_loop_run", runLoop) &&
               loadSymbol(library, "g_main_loop_quit", quitLoop) && loadSymbol(library, "g_error_free", freeError);
    }
};

constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kShortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";

// Remembers that the user said no to the shortcut, so they aren't asked at every launch.
std::string hotkeyDeclinedMarkerPath() {
    const std::string settingsPath = settingsFilePath();
    return settingsPath.empty() ? "" : fs::path(settingsPath).parent_path().string() + "/hotkey-declined";
}

// Tells the portal who we are (matching veeastats.desktop). This has to be the
// first portal call on the D-Bus connection: after any other, the portal has
// already filed VeeaStats as an unknown app, and the GlobalShortcuts portal
// turns unknown apps away. The shortcut and appearance code share one
// connection (GLib's session bus), so whichever gets there first does it.
// Flatpak and Snap apps are identified without this, and older portals lack it.
void registerWithPortal(const GioApi& gio, GDBusConnection* bus) {
    static std::once_flag once;
    std::call_once(once, [&] {
        GError* error = nullptr;
        GVariant* reply = gio.callSync(bus, kPortalBusName, kPortalPath, "org.freedesktop.host.portal.Registry", "Register",
                                       gio.newParsed("('veeastats', @a{sv} {})"), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        if (reply) gio.unref(reply);
        else       gio.freeError(error);
    });
}

class ShortcutsPortal {
public:
    // Registers Ctrl+Shift+O with the desktop, then posts kRequestToggleOverlay
    // every time it is pressed. Runs for as long as VeeaStats does (so on a
    // thread of its own), unless there's no portal or the shortcut isn't bound.
    void run() {
        if (!gio_.load()) return;
        GMainContext* context = gio_.newContext();
        gio_.pushContext(context);   // portal answers are delivered to this thread
        loop_ = gio_.newLoop(context, FALSE);

        GError* error = nullptr;
        bus_ = gio_.busGetSync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!bus_) {
            gio_.freeError(error);
            return;
        }
        // Request objects are named after our D-Bus name: ":1.42" -> "1_42".
        sender_ = gio_.uniqueName(bus_) + 1;
        std::replace(sender_.begin(), sender_.end(), '.', '_');

        registerWithPortal(gio_, bus_);

        std::string token = nextToken();
        GVariant* results = request("CreateSession", gio_.newParsed("({'handle_token': <%s>, 'session_handle_token': <%s>},)",
                                                                    token.c_str(), "veeastats"), token);
        if (!results) return;   // no GlobalShortcuts portal on this desktop
        // Everything the portal sends is checked before use: the session handle
        // goes into "%o" arguments below, and GLib aborts the whole program on a
        // string there that isn't a valid object path.
        if (GVariant* handle = gio_.lookupValue(results, "session_handle", nullptr)) {
            const std::string type = gio_.typeString(handle);
            if (type == "s" || type == "o") {
                const char* text = nullptr;
                gio_.get(handle, type == "s" ? "&s" : "&o", &text);
                if (text && gio_.isObjectPath(text)) session_ = text;
            }
            gio_.unref(handle);
        }
        gio_.unref(results);
        if (session_.empty()) return;

        if (!alreadyBound(session_) && !bind(session_)) return;

        // Activated's first argument is our session, but as an object path, and
        // D-Bus "arg0" filters only match plain strings. So it's checked here.
        gio_.signalSubscribe(
            bus_, kPortalBusName, kShortcutsInterface, "Activated", kPortalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
                ShortcutsPortal& portal = *static_cast<ShortcutsPortal*>(data);
                const gchar* session = nullptr;
                const gchar* shortcut = nullptr;
                guint64 timestamp = 0;
                GVariant* options = nullptr;
                if (strcmp(portal.gio_.typeString(parameters), "(osta{sv})") != 0) return;   // not a signal we understand
                portal.gio_.get(parameters, "(&o&st@a{sv})", &session, &shortcut, &timestamp, &options);
                if (session == portal.session_ && strcmp(shortcut, "toggle-overlay") == 0) postRequest(kRequestToggleOverlay);
                portal.gio_.unref(options);
            },
            this, nullptr);
        gio_.runLoop(loop_);
    }

private:
    GVariant* call(const char* interface, const char* method, GVariant* parameters) {
        GError* error = nullptr;
        GVariant* reply = gio_.callSync(bus_, kPortalBusName, kPortalPath, interface, method, parameters, nullptr,
                                        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (!reply) gio_.freeError(error);
        return reply;
    }

    std::string nextToken() { return "veeastats" + std::to_string(++tokenCount_); }

    // Portal methods answer later, through a "Response" signal on a Request
    // object named after `token`. Calls `method` and waits for that answer.
    // Returns its results (unref them when done), or nullptr if the call failed
    // or the user said no. `cancelled` is set only when the user said no
    // (response 1), not when the desktop or the call failed (2).
    GVariant* request(const char* method, GVariant* parameters, const std::string& token, bool* cancelled = nullptr) {
        struct Answer {
            GioApi* gio;
            GMainLoop* loop;
            guint32 code{2};
            GVariant* results{nullptr};
            bool answered{false};   // only the first Response counts (a second one would leak the first's results)
        } answer{&gio_, loop_};

        const std::string path = std::string(kPortalPath) + "/request/" + sender_ + "/" + token;
        const guint subscription = gio_.signalSubscribe(
            bus_, kPortalBusName, "org.freedesktop.portal.Request", "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
                Answer& answer = *static_cast<Answer*>(data);
                if (answer.answered) return;
                answer.answered = true;
                if (strcmp(answer.gio->typeString(parameters), "(ua{sv})") == 0) {
                    answer.gio->get(parameters, "(u@a{sv})", &answer.code, &answer.results);
                }
                answer.gio->quitLoop(answer.loop);
            },
            &answer, nullptr);

        if (GVariant* reply = call(kShortcutsInterface, method, parameters)) {
            gio_.unref(reply);
            gio_.runLoop(loop_);   // until the Response arrives (for BindShortcuts, until the user answers)
        }
        gio_.signalUnsubscribe(bus_, subscription);

        if (cancelled) *cancelled = (answer.code == 1);
        if (answer.code != 0 && answer.results) {
            gio_.unref(answer.results);
            answer.results = nullptr;
        }
        return answer.results;
    }

    // True if the desktop already has our shortcut from an earlier run.
    bool alreadyBound(const std::string& session) {
        const std::string token = nextToken();
        GVariant* results = request("ListShortcuts", gio_.newParsed("(%o, {'handle_token': <%s>})", session.c_str(), token.c_str()), token);
        if (!results) return false;
        // With the expected type given, a value of any other type comes back as
        // nullptr (counting the children of a non-container would abort).
        GVariant* shortcuts = gio_.lookupValue(results, "shortcuts", variantType("a(sa{sv})"));
        const bool bound = shortcuts && gio_.childCount(shortcuts) > 0;
        if (shortcuts) gio_.unref(shortcuts);
        gio_.unref(results);
        return bound;
    }

    // Asks the desktop for Ctrl+Shift+O. The desktop shows the user a
    // confirmation (where they may also pick a different key).
    bool bind(const std::string& session) {
        const std::string declinedMarker = hotkeyDeclinedMarkerPath();
        if (declinedMarker.empty() || fileExists(declinedMarker)) return false;

        const std::string token = nextToken();
        bool cancelled = false;
        GVariant* results = request(
            "BindShortcuts",
            gio_.newParsed("(%o, [('toggle-overlay', {'description': <'Show or hide the VeeaStats overlay'>, "
                           "'preferred_trigger': <'CTRL+SHIFT+o'>})], '', {'handle_token': <%s>})",
                           session.c_str(), token.c_str()),
            token, &cancelled);
        if (!results && !cancelled) return false;   // the desktop failed, not the user: try again next launch
        if (!results) {   // the user said no: don't ask at every launch
            std::error_code ignored;
            fs::create_directories(fs::path(declinedMarker).parent_path(), ignored);
            if (FILE* file = fopen(declinedMarker.c_str(), "w")) fclose(file);
            return false;
        }
        gio_.unref(results);
        return true;
    }

    GioApi gio_;
    GDBusConnection* bus_{nullptr};
    GMainLoop* loop_{nullptr};
    std::string sender_;
    std::string session_;
    int tokenCount_{0};
};

void runShortcutsPortal() {
    ShortcutsPortal portal;
    portal.run();
}

// ---- The desktop's light/dark style and accent colour --------------------------
//
// The GNOME skin follows the desktop, as GNOME apps do. Both settings come from
// the desktop's Settings portal (GNOME, KDE and others have one), which also
// announces when the user changes them. Without a portal the skin stays light,
// in GNOME's default blue.

std::atomic<bool> g_desktopDark{false};
std::atomic<unsigned> g_desktopAccent{0x3584e4};   // 0xRRGGBB

class AppearanceWatcher {
public:
    // Reads both settings now, then watches for changes on a thread of its own.
    void start() {
        if (!gio_.load()) return;
        GError* error = nullptr;
        bus_ = gio_.busGetSync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!bus_) {
            gio_.freeError(error);
            return;
        }
        registerWithPortal(gio_, bus_);   // before reading anything (see there)
        for (const char* key : {"color-scheme", "accent-color"}) {
            if (GVariant* value = read(key)) {
                store(key, value);
                gio_.unref(value);
            }
        }
        std::thread([this] { watch(); }).detach();
    }

private:
    GVariant* call(const char* method, const char* key) {
        GError* error = nullptr;
        GVariant* reply = gio_.callSync(bus_, kPortalBusName, kPortalPath, "org.freedesktop.portal.Settings", method,
                                        gio_.newParsed("('org.freedesktop.appearance', %s)", key), nullptr, G_DBUS_CALL_FLAGS_NONE,
                                        1000, nullptr, &error);
        if (!reply) gio_.freeError(error);
        return reply;
    }

    // The value of an org.freedesktop.appearance setting (unref it when done), or
    // nullptr. Each reply's type is checked first: unpacking a reply of another
    // shape would leave the pointers empty.
    GVariant* read(const char* key) {
        const auto isVariantReply = [&](GVariant* reply) { return strcmp(gio_.typeString(reply), "(v)") == 0; };
        GVariant* value = nullptr;
        if (GVariant* reply = call("ReadOne", key)) {
            if (isVariantReply(reply)) gio_.get(reply, "(v)", &value);
            gio_.unref(reply);
        } else if (GVariant* oldReply = call("Read", key)) {   // older portals: the value is wrapped twice
            if (isVariantReply(oldReply)) {
                GVariant* wrapped = nullptr;
                gio_.get(oldReply, "(v)", &wrapped);
                if (strcmp(gio_.typeString(wrapped), "v") == 0) gio_.get(wrapped, "v", &value);
                gio_.unref(wrapped);
            }
            gio_.unref(oldReply);
        }
        return value;
    }

    void store(const char* key, GVariant* value) {
        const std::string type = gio_.typeString(value);
        if (strcmp(key, "color-scheme") == 0 && type == "u") {
            guint32 scheme = 0;
            gio_.get(value, "u", &scheme);
            g_desktopDark = (scheme == 1);   // 1 = prefers dark; 0 (no preference) and 2 = light
        } else if (strcmp(key, "accent-color") == 0 && type == "(ddd)") {
            double red = -1.0, green = -1.0, blue = -1.0;
            gio_.get(value, "(ddd)", &red, &green, &blue);
            const auto valid = [](double c) { return c >= 0.0 && c <= 1.0; };   // outside 0..1 means "not set"
            if (valid(red) && valid(green) && valid(blue)) {
                const auto byte = [](double c) { return static_cast<unsigned>(c * 255.0 + 0.5); };
                g_desktopAccent = byte(red) << 16 | byte(green) << 8 | byte(blue);
            }
        }
    }

    void watch() {
        GMainContext* context = gio_.newContext();
        gio_.pushContext(context);   // changes are delivered to this thread
        GMainLoop* loop = gio_.newLoop(context, FALSE);
        gio_.signalSubscribe(
            bus_, kPortalBusName, "org.freedesktop.portal.Settings", "SettingChanged", kPortalPath, "org.freedesktop.appearance",
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
                AppearanceWatcher& watcher = *static_cast<AppearanceWatcher*>(data);
                const gchar* settingsNamespace = nullptr;
                const gchar* key = nullptr;
                GVariant* value = nullptr;
                if (strcmp(watcher.gio_.typeString(parameters), "(ssv)") != 0) return;   // not a signal we understand
                watcher.gio_.get(parameters, "(&s&sv)", &settingsNamespace, &key, &value);
                watcher.store(key, value);
                watcher.gio_.unref(value);
                postRequest(kRequestAppearance);
            },
            this, nullptr);
        gio_.runLoop(loop);
    }

    GioApi gio_;
    GDBusConnection* bus_{nullptr};
};

// ---- One copy at a time ------------------------------------------------------
//
// Opening VeeaStats while it is already running (for example from the app menu
// while it runs in the background) shows the running copy's window instead of
// starting a second one. If the running copy is a different build, because the
// user opened a newer AppImage, the old copy quits and the new one takes over.
// The copies talk through a Unix socket with an abstract name (no file on disk).

// A plain constant, not a std::string: the listener thread can still be
// answering while the program exits, after strings have been destroyed.
constexpr const char kShowThisBuild[] = "show " __DATE__ " " __TIME__;

sockaddr_un instanceSocketAddress(socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string name = "veeastats-" + std::to_string(getuid());   // one per user
    memcpy(address.sun_path + 1, name.data(), name.size());              // leading '\0' = abstract
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return address;
}

// Reads one line, without the newline: at most 256 characters and within
// `timeoutMs` in total, so a stuck or hostile client can't hold the caller.
std::string readSocketLine(int fd, int timeoutMs) {
    constexpr size_t kMaxLine = 256;
    const double deadline = monotonicMs() + timeoutMs;
    std::string line;
    char c;
    while (line.size() < kMaxLine) {
        const int remainingMs = static_cast<int>(deadline - monotonicMs());
        pollfd waitForData{fd, POLLIN, 0};
        if (remainingMs <= 0 || poll(&waitForData, 1, remainingMs) <= 0 || read(fd, &c, 1) != 1 || c == '\n') break;
        line += c;
    }
    return line;
}

// True if the other end of `fd` is a process of this same user. Abstract
// sockets have no file permissions, so any local user could connect to (or
// take the name of) ours; only copies run by this user are listened to.
bool peerIsThisUser(int fd) {
    ucred peer{};
    socklen_t length = sizeof(peer);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0 && peer.uid == getuid();
}

// Sends `message` to the running copy and returns its reply ("" if none is
// running, or if it doesn't answer within a few seconds).
std::string messageRunningCopy(const std::string& message) {
    socklen_t length;
    const sockaddr_un address = instanceSocketAddress(length);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "";
    // A copy that is stopped (Ctrl+Z, a frozen cgroup) never accepts, and once
    // its queue is full connect() would wait forever; this limits that wait.
    const timeval connectLimit{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &connectLimit, sizeof(connectLimit));
    std::string reply;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0 && peerIsThisUser(fd)) {
        const std::string line = message + "\n";
        // MSG_NOSIGNAL: a copy that quits just now must not kill this one with SIGPIPE.
        if (send(fd, line.data(), line.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(line.size())) reply = readSocketLine(fd, 2000);
    }
    close(fd);
    return reply;
}

// Returns a listening socket if this is now the only copy, or -1. Sets
// `handedOver` when a running copy is showing its window instead, in which case
// this copy should just exit.
int claimOnlyCopy(bool& handedOver) {
    socklen_t length;
    const sockaddr_un address = instanceSocketAddress(length);
    const double deadline = monotonicMs() + 6000.0;   // a copy that never answers can't hold up the launch for long
    for (int attempt = 0; attempt < 30 && monotonicMs() < deadline; ++attempt) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (bind(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0 && listen(fd, 4) == 0) return fd;
        close(fd);

        if (messageRunningCopy(kShowThisBuild) == "ok") {
            handedOver = true;
            return -1;
        }
        usleep(100 * 1000);   // the old copy is quitting (or just did); try again shortly
    }
    return -1;   // couldn't sort it out: run anyway
}

// Answers other copies: "show <build>" and "quit". Runs on a thread of its own
// until main() shuts the socket down on the way out.
void listenForOtherCopies(int listenFd) {
    while (true) {
        const int client = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINVAL || errno == EBADF || errno == ENOTSOCK) return;   // shut down: VeeaStats is quitting
            // Anything else (EINTR, out of file descriptors or memory, a client
            // that gave up) passes: stopping here would leave the name taken
            // with nobody answering, and every launch would wait on it.
            if (errno != EINTR) usleep(100 * 1000);
            continue;
        }
        if (!peerIsThisUser(client)) {
            close(client);
            continue;
        }
        const std::string message = readSocketLine(client, 1000);
        std::string reply;
        if (message == kShowThisBuild) {
            reply = "ok\n";
            postRequest(kRequestShowWindow);
        } else if (startsWith(message, "show ")) {   // a different build: make way for it
            reply = "replacing\n";
            postRequest(kRequestQuit);
        } else if (message == "quit") {
            reply = "ok\n";
            postRequest(kRequestQuit);
        }
        if (!reply.empty() && send(client, reply.data(), reply.size(), MSG_NOSIGNAL) < 0) {
            // the other copy gave up waiting; nothing to do
        }
        close(client);
    }
}

}  // namespace

// ============================================================================
// 9. MAIN
// ============================================================================

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "";
    if (mode == "--overlay") return runOverlayProcess();
    if (mode == "--quit") {   // used by the uninstaller
        messageRunningCopy("quit");
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);   // writing to an overlay that has quit must not end the program

    // Graphics first: a different build that's already running quits to make
    // way for this one (see claimOnlyCopy), so this copy must know it can run
    // (a display to show its window on, for one) before it asks for that.
    glfwSetErrorCallback(onGlfwError);
    if (!glfwInit()) return 1;

    bool handedOver = false;
    const int instanceSocket = claimOnlyCopy(handedOver);
    if (handedOver) return 0;   // VeeaStats was already running and is showing its window instead
    // Answer other copies straight away, not once startup has finished: a launch
    // meanwhile would otherwise wait, give up and run as a second copy. Their
    // requests are only stored until the main loop starts.
    if (instanceSocket >= 0) std::thread(listenForOtherCopies, instanceSocket).detach();

#if defined(IMGUI_IMPL_OPENGL_ES3)
    const char* glslVersion = "#version 300 es";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#else
    const char* glslVersion = "#version 130";
#endif
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // Lets the desktop match the window to veeastats.desktop (taskbar icon/name).
#if defined(GLFW_WAYLAND_APP_ID)
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "veeastats");
#endif
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "veeastats");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "veeastats");

    // Preferred window size, shrunk to fit small screens such as handhelds.
    int width = 920, height = 720;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr) {
        width  = std::min(width, mode->width);
        height = std::min(height, mode->height);
    }

    // Read before the window opens, so a borderless window never flashes its border.
    const Settings savedSettings = loadSettings();
    glfwWindowHint(GLFW_DECORATED, savedSettings.borderless ? GLFW_FALSE : GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(width, height, "VeeaStats", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    // No vsync: the main loop paces its own frames. With vsync, a window that is
    // fully covered (by a fullscreen game, say) gets stuck waiting for a screen
    // refresh the desktop never sends it, and stops updating the overlay.
    glfwSwapInterval(0);

    bool inputArrived = false;   // set by the callbacks whenever the user does something
    watchForInput(window, &inputArrived);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // don't write imgui.ini to disk
    io.LogFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    const UiFonts fonts = loadFonts();
    UiState ui;                                   // changed by the controls in the window
    ui.settings = savedSettings;
    static AppearanceWatcher appearance;          // static: its thread uses it until the program ends
    appearance.start();
    setGnomeAppearance(g_desktopDark, rgbColor(g_desktopAccent));
    applySkin(ui.settings.skin, fonts);
    Settings appliedSettings = ui.settings;       // what the window currently looks like
    const HudSkins hudSkins = makeHudSkins(fonts);
    HudState hud;

    // The hardware details are read on a helper thread (counting installed
    // packages can take a second or two), so the window draws straight away and
    // fills them in when they arrive.
    std::promise<StaticInfo> staticInfoPromise;
    std::future<StaticInfo> staticInfoReady = staticInfoPromise.get_future();
    std::thread([promise = std::move(staticInfoPromise)]() mutable {
        promise.set_value(readStaticInfo());
        postRequest(kRequestStaticInfo);
    }).detach();
    StaticInfo staticInfo;   // shown until then
    staticInfo.cpuModel = staticInfo.board.name = staticInfo.board.chipset = "Reading...";
    staticInfo.os = {"Reading...", "", "", ""};

    LiveMonitor monitorLive(ui.settings.refreshMs);
    double lastRefresh = glfwGetTime();
    int framesToDraw = 2;                         // frames to draw back-to-back before sleeping (see below)

    {
        std::lock_guard<std::mutex> lock(g_glfwLock);
        g_glfwRunning = true;   // from here on the helper threads may wake the main loop
    }
    // On X11 sessions the overlay's own key grab already works in every program.
    if (getenv("WAYLAND_DISPLAY")) std::thread(runShortcutsPortal).detach();
    OverlayProcess overlay;
    overlay.start();
    overlay.setSkin(ui.settings.skin);
    bool windowHidden = false;                    // closed while running in the background

    while (true) {
        const unsigned requests = g_requests.exchange(0);
        if (requests & kRequestQuit) break;
        if (staticInfoReady.valid() && staticInfoReady.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            staticInfo = staticInfoReady.get();
            framesToDraw = 2;
        }
        if (requests & kRequestRedraw) framesToDraw = 2;
        if (requests & kRequestAppearance) {      // the desktop switched between light and dark, or changed accent
            setGnomeAppearance(g_desktopDark, rgbColor(g_desktopAccent));
            if (ui.settings.skin == Skin::Gnome) applySkin(ui.settings.skin, fonts);
            framesToDraw = 2;
        }
        if (requests & kRequestShowWindow) {      // VeeaStats was opened again
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) glfwRestoreWindow(window);
            glfwShowWindow(window);
            glfwFocusWindow(window);
            windowHidden = false;
            framesToDraw = 2;
        }
        if ((requests & kRequestOverlayEnded) || overlay.secondsUntilRetry() == 0.0) overlay.restartIfEnded();
        if (requests & kRequestToggleOverlay) {
            // Fresh numbers in case it's opening, unless they're fresh already:
            // CPU load measured over a few milliseconds is just 0% or 100%.
            if (glfwGetTime() - lastRefresh >= 0.25) {
                monitorLive.refresh();
                lastRefresh = glfwGetTime();
            }
            overlay.toggle(monitorLive.stats(), ui.settings.limitOverlayToApp, ui.settings.fahrenheit);
        }

        if (ui.quitRequested) break;
        if (glfwWindowShouldClose(window)) {
            if (!ui.settings.runInBackground) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);   // in the background, closing only hides the window
            glfwHideWindow(window);
            windowHidden = true;
            ui.settingsOpen = false;
        }

        // Stats are only read while someone can see them: in the window, or in the overlay.
        const bool drawing = !windowHidden && !glfwGetWindowAttrib(window, GLFW_ICONIFIED);
        const double frameStart = glfwGetTime();
        if ((drawing || overlay.active()) && frameStart - lastRefresh >= ui.settings.refreshMs / 1000.0) {
            monitorLive.refresh();
            lastRefresh = glfwGetTime();
            if (overlay.active()) overlay.sendStats(monitorLive.stats(), ui.settings.fahrenheit);
        }

        if (drawing) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            drawWindow(io.DisplaySize, ui, hudSkins, hud, staticInfo, monitorLive.stats());
            ImGui::Render();

            if (ui.settings != appliedSettings) {     // the user changed something in the settings menu
                if (ui.settings.skin != appliedSettings.skin) {
                    applySkin(ui.settings.skin, fonts);
                    overlay.setSkin(ui.settings.skin);
                    framesToDraw = 2;                 // lay the new skin out straight away
                }
                if (ui.settings.refreshMs != appliedSettings.refreshMs) monitorLive.setRefreshIntervalMs(ui.settings.refreshMs);
                if (ui.settings.borderless != appliedSettings.borderless) {
                    glfwSetWindowAttrib(window, GLFW_DECORATED, ui.settings.borderless ? GLFW_FALSE : GLFW_TRUE);
                }
                if (ui.settings.fahrenheit != appliedSettings.fahrenheit && overlay.active()) {
                    overlay.sendStats(monitorLive.stats(), ui.settings.fahrenheit);   // switch its units now, not at the next refresh
                }
                saveSettings(ui.settings);
                appliedSettings = ui.settings;
            }

            int framebufferWidth, framebufferHeight;
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            glViewport(0, 0, framebufferWidth, framebufferHeight);
            glClearColor(0.1f, 0.105f, 0.11f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        // Sleep until the next refresh is due or the user interacts with the
        // window (mouse, keyboard, resize...). Nothing changes in between, so
        // there is no reason to redraw 60 times a second.
        //
        // After the very first frame, and after every user action, two frames are
        // drawn back-to-back: ImGui needs a second pass to finish laying out
        // tables and to show a freshly opened dropdown.
        //
        // Animated skins are the exception: they redraw at a steady 30 fps
        // (still only a sliver of one CPU core).
        //
        // With the window minimized or hidden, only the overlay (if showing)
        // needs waking for; otherwise sleep until something happens.
        const double untilRefresh = std::max(ui.settings.refreshMs / 1000.0 - (glfwGetTime() - lastRefresh), 0.0);
        double timeout = -1.0;                        // no timeout
        if (drawing) {
            if (framesToDraw > 0) --framesToDraw;
            timeout = (framesToDraw > 0) ? 0.0 : untilRefresh;
            if (ui.settings.skin != Skin::Classic && ui.settings.animations) {
                timeout = std::min(timeout, std::max(frameStart + kAnimationFrameSeconds - glfwGetTime(), 0.0));
            }
        } else if (overlay.active()) {
            timeout = untilRefresh;
        }
        if (const double retry = overlay.secondsUntilRetry(); retry >= 0.0) {   // an overlay restart is waiting
            timeout = (timeout < 0.0) ? retry : std::min(timeout, retry);
        }

        // Sleep until the timeout, but wake straight away for user input or a
        // request from another thread. Any other wake-up just goes back to sleep
        // for the time that's left.
        inputArrived = false;
        const double deadline = glfwGetTime() + timeout;
        const auto keepSleeping = [&] {
            return !inputArrived && g_requests == 0 && !glfwWindowShouldClose(window) &&
                   (timeout < 0.0 || glfwGetTime() < deadline);
        };
        if (timeout < 0.0) glfwWaitEvents();
        else               glfwWaitEventsTimeout(timeout);
        while (keepSleeping()) {
            if (timeout < 0.0) glfwWaitEvents();
            else               glfwWaitEventsTimeout(std::max(deadline - glfwGetTime(), 0.0));   // never negative: GLFW rejects that
        }
        if (inputArrived) framesToDraw = 2;
    }

    {
        std::lock_guard<std::mutex> lock(g_glfwLock);
        g_glfwRunning = false;   // no more wake-ups from the helper threads
    }
    // Stop answering other copies: one opened from now on gets "nobody here"
    // instead of an "ok" from a window that's about to close, and the listener
    // thread ends. (Not closed: the thread may still be using the descriptor.)
    if (instanceSocket >= 0) shutdown(instanceSocket, SHUT_RDWR);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
