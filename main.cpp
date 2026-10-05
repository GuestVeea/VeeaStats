// VeeaStats - a small Linux hardware monitor built with Dear ImGui + GLFW + OpenGL.
//
// How this file is organised:
//   1. Helpers         - reading files, parsing numbers, formatting text
//   2. Data types      - plain structs that hold what we display
//   3. Static info     - read ONCE at startup (CPU name, drives, OS, ...)
//   4. Live stats      - re-read every update interval (CPU load, RAM, GPU, ...)
//   5. UI              - settings, shared widgets and the Classic skin
//   6. HUD skins       - JARV, Galactic Conflict, Federation Gunship and
//                        Halloween: one shared layout, each skin drawing it its own way
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
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <gio/gio.h>

// Fonts for the HUD skins, embedded so nothing needs installing (SIL Open Font
// License; see fonts/). Trimmed to Western European characters to keep them small.
#include "fonts/fredoka_semibold.h"
#include "fonts/hennypenny_regular.h"
#include "fonts/michroma_regular.h"
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
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
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

// True if `name` is an executable somewhere on $PATH (replaces `which`).
bool commandExists(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return false;

    std::string_view dirs(pathEnv);
    while (!dirs.empty()) {
        const size_t colon = dirs.find(':');
        const std::string dir(dirs.substr(0, colon));
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if (colon == std::string_view::npos) break;
        dirs.remove_prefix(colon + 1);
    }
    return false;
}

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
    if (end == text.c_str()) return false;
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
    unsigned long long active{0};
    unsigned long long total{0};
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
    std::vector<float> cpuPercent;   // one entry per core
    double cpuFreqGhz{0.0};
    double cpuVoltage{0.0};
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
            if (digitCount < 2 || digitCount > 3) continue;

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

// DIMM details live in the BIOS tables, which only root can read, so this needs
// dmidecode run as root (or via passwordless sudo). If that isn't possible the
// UI just shows "Standard System RAM".
std::vector<std::string> readRamModuleLabels() {
    const char* command = (geteuid() == 0) ? "dmidecode -t memory 2>/dev/null"
                                           : "sudo -n dmidecode -t memory 2>/dev/null";
    std::vector<RamModule> modules;
    RamModule current;
    bool inDevice = false;

    auto finishDevice = [&]() {
        const bool empty = current.partNumber.empty() || current.partNumber == "NO DIMM" ||
                           startsWith(current.size, "No Module");
        if (inDevice && !empty) modules.push_back(current);
        current = RamModule();
    };

    forEachCommandLine(command, [&](const char* line) {
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
        if (strcmp(line, "Status: install ok installed") == 0) ++count;
        return kKeepGoing;
    });
    return count;
}

int countRpm() {
    // The rpm database is a binary format, so ask rpm itself.
    if (!commandExists("rpm")) return 0;
    int count = 0;
    forEachCommandLine("rpm -qa 2>/dev/null", [&](const char*) {
        ++count;
        return kKeepGoing;
    });
    return count;
}

int countFlatpak() {
    // Counts installed apps (not runtimes), system-wide and per-user.
    int count = countSubdirectories("/var/lib/flatpak/app");   // note: only app *ids*, not arch/branch
    const char* dataHome = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (dataHome && *dataHome)  count += countSubdirectories(std::string(dataHome) + "/flatpak/app");
    else if (home && *home)     count += countSubdirectories(std::string(home) + "/.local/share/flatpak/app");
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

        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        sscanf(line, "cpu%*u %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

        CpuTimes times;
        times.active = user + nice + system + irq + softirq + steal;
        times.total  = times.active + idle + iowait;   // guest time is already counted inside user/nice
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

// Finds the sensor files that might report CPU core voltage, best guess first.
// Done once at startup so each refresh only reads a couple of tiny files.
//   1. Any voltage whose label names the CPU core (zenpower "SVI2_Core",
//      asus-ec-sensors "CPU Core", ...).
//   2. in0 of a motherboard Super I/O chip (Nuvoton "nct6799", ITE "it8686",
//      ...). Boards wire in0 to Vcore; Ryzen 7000+ CPUs report no voltage of
//      their own, so on those this is the only source.
//   3. Unlabelled inputs of CPU sensors.
std::vector<std::string> findCpuVoltageInputs() {
    static const char* const kCpuSensorNames[] = {"k10temp", "zenpower", "coretemp", "cpu_thermal"};
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
// moment and wastes CPU); a single process that streams a new line at our
// update interval avoids that completely. If the user picks a different
// interval, start() is simply called again to replace the process.
class NvidiaSmiStream {
public:
    NvidiaSmiStream() = default;
    NvidiaSmiStream(const NvidiaSmiStream&) = delete;
    NvidiaSmiStream& operator=(const NvidiaSmiStream&) = delete;
    ~NvidiaSmiStream() { stop(); }

    bool running() const { return fd_ >= 0; }

    // Starts (or restarts) nvidia-smi so it prints a line every `intervalMs`.
    // `waitForFirstLineMs` is how long to wait for that first line: wanted at
    // program start so the first frame has data, but 0 when restarting so the
    // window never stalls (the old numbers stay on screen until a new line arrives).
    bool start(int intervalMs, int waitForFirstLineMs) {
        stop();
        buffer_.clear();

        int pipeEnds[2];
        if (pipe(pipeEnds) != 0) return false;

        // Child process: stdout -> our pipe, stderr -> /dev/null.
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, pipeEnds[0]);
        posix_spawn_file_actions_addclose(&actions, pipeEnds[1]);

        const std::string loopArgument = "--loop-ms=" + std::to_string(intervalMs);
        const char* const args[] = {
            "nvidia-smi", "-i", "0",   // first GPU only
            "--query-gpu=gpu_name,utilization.gpu,memory.used,memory.total,clocks.current.graphics",
            "--format=csv,noheader,nounits",
            loopArgument.c_str(),
            nullptr};
        const int result = posix_spawnp(&pid_, "nvidia-smi", &actions, nullptr, const_cast<char* const*>(args), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipeEnds[1]);

        if (result != 0) {   // nvidia-smi isn't installed
            close(pipeEnds[0]);
            pid_ = -1;
            return false;
        }

        fd_ = pipeEnds[0];
        fcntl(fd_, F_SETFL, O_NONBLOCK);   // reading must never freeze the UI
        fcntl(fd_, F_SETFD, FD_CLOEXEC);

        if (waitForFirstLineMs > 0) {
            pollfd waitForData{fd_, POLLIN, 0};
            poll(&waitForData, 1, waitForFirstLineMs);
        }
        return true;
    }

    // Returns the newest complete line printed since the last call, or "" if none.
    std::string latestLine() {
        if (fd_ < 0) return "";

        char chunk[512];
        ssize_t bytes;
        while ((bytes = read(fd_, chunk, sizeof(chunk))) > 0) buffer_.append(chunk, static_cast<size_t>(bytes));
        if (bytes == 0) stop();   // nvidia-smi exited

        const size_t lastNewline = buffer_.rfind('\n');
        if (lastNewline == std::string::npos) return "";
        size_t lineStart = (lastNewline == 0) ? std::string::npos : buffer_.rfind('\n', lastNewline - 1);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;

        std::string line = buffer_.substr(lineStart, lastNewline - lineStart);
        buffer_.erase(0, lastNewline + 1);
        return line;
    }

private:
    void stop() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

    pid_t pid_{-1};
    int fd_{-1};
    std::string buffer_;
};

// Parses one line like "NVIDIA GeForce RTX 3080, 12, 1500, 10240, 1800"
// (name, GPU %, VRAM used MiB, VRAM total MiB, clock MHz).
GpuStats parseNvidiaLine(const std::string& line) {
    GpuStats gpu;
    gpu.vendor = GpuVendor::Nvidia;

    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t comma = line.find(',', start);
        fields.push_back(trim(line.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (fields.size() < 5) return gpu;

    double usage = 0.0, usedMb = 0.0, totalMb = 0.0, clockMhz = 0.0;
    if (!parseNumber(fields[1], usage)) return gpu;

    gpu.name = fields[0];
    gpu.gpuUsage = static_cast<float>(usage);
    gpu.hasLoad = true;
    if (parseNumber(fields[2], usedMb) && parseNumber(fields[3], totalMb) && totalMb > 0.0) {
        gpu.hasMemory = true;
        gpu.vramUsedGb  = usedMb / 1024.0;
        gpu.vramTotalGb = totalMb / 1024.0;
        gpu.vramUsage   = static_cast<float>(100.0 * usedMb / totalMb);
    }
    if (parseNumber(fields[4], clockMhz)) gpu.clockGhz = clockMhz / 1000.0;
    return gpu;
}

// ---- GPU: helpers shared by the AMD / Intel / Mali readers -------------------

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
    std::string busyPath, vramUsedPath, clockPath, voltagePath;
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
        gpu.loadNote = "Core load: not reported by driver";
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

    for (const std::string& path : card.clockPaths) {   // a reading of 0 means "asleep right now", so try the next file
        double mhz = 0.0;
        if (readNumber(path, mhz) && mhz > 0.0) {
            gpu.clockGhz = mhz / 1000.0;
            break;
        }
    }

    double idleMs = 0.0;
    if (card.idleCounterPath.empty() || !readNumber(card.idleCounterPath, idleMs)) {
        gpu.loadNote = "Core load: not reported by driver";
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
    return gpu;
}

GpuStats readMaliGpu(const MaliGpu& mali) {
    GpuStats gpu;
    gpu.vendor = GpuVendor::Mali;
    gpu.name = mali.name;
    gpu.memoryNote = "VRAM: shares system RAM";

    double value = 0.0;
    if (!mali.clockPath.empty() && readNumber(mali.clockPath, value)) gpu.clockGhz = value / 1e9;   // devfreq reports Hz

    // "load" files look like "23@800000000Hz" (load, then clock) and "utilisation"
    // files are a plain "23". Reading the number at the start handles both.
    if (!mali.loadPath.empty() && readNumber(mali.loadPath, value) && value >= 0.0 && value <= 100.0) {
        gpu.gpuUsage = static_cast<float>(value);
        gpu.hasLoad = true;
    } else {
        gpu.loadNote = "Core load: not reported by driver";
    }
    return gpu;
}

// ---- GPU: pick the right reader and give the UI one simple interface --------

bool hasNvidiaCard(const std::string& sysRoot) {
    for (const std::string& cardDir : listGpuCardDirs(sysRoot)) {
        if (readFirstLine(cardDir + "/device/vendor") == kNvidiaVendorId) return true;
    }
    return false;
}

class GpuMonitor {
public:
    explicit GpuMonitor(const std::string& sysRoot = "/sys") {
        amdCards_   = findAmdCards(sysRoot);
        intelCards_ = findIntelCards(sysRoot);
        mali_       = findMaliGpu(sysRoot);

        // If a machine has several kinds of GPU (typical for laptops) the most
        // powerful one is shown: NVIDIA, then AMD, then Intel, then Mali.
        if (fileExists("/proc/driver/nvidia/gpus") || commandExists("nvidia-smi")) vendor_ = GpuVendor::Nvidia;
        else if (!amdCards_.empty())   vendor_ = GpuVendor::Amd;
        else if (!intelCards_.empty()) vendor_ = GpuVendor::Intel;
        else if (hasNvidiaCard(sysRoot)) vendor_ = GpuVendor::Nvidia;   // open "nouveau" driver: no stats, but we know the brand
        else if (mali_)                vendor_ = GpuVendor::Mali;

        nvidiaStats_.vendor = GpuVendor::Nvidia;
        if (vendor_ == GpuVendor::Nvidia) nvidia_.start(nvidiaIntervalMs_, 2000);
    }

    // Called when the user picks a new update interval. Only NVIDIA needs to
    // react: its nvidia-smi helper is restarted at the new rate. The other
    // GPUs are read directly from files each refresh.
    void setRefreshIntervalMs(int intervalMs) {
        if (vendor_ != GpuVendor::Nvidia || intervalMs == nvidiaIntervalMs_) return;
        nvidiaIntervalMs_ = intervalMs;
        nvidia_.start(intervalMs, 0);
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
    GpuStats readNvidia() {
        const std::string line = nvidia_.latestLine();
        if (!line.empty()) nvidiaStats_ = parseNvidiaLine(line);
        if (!nvidia_.running()) {   // nvidia-smi is not installed or has stopped
            nvidiaStats_.hasLoad = false;
            nvidiaStats_.hasMemory = false;
        }
        return nvidiaStats_;
    }

    GpuVendor vendor_{GpuVendor::Unknown};
    int nvidiaIntervalMs_{kDefaultRefreshMs};
    NvidiaSmiStream nvidia_;
    GpuStats nvidiaStats_;
    std::vector<AmdCard> amdCards_;
    std::vector<IntelCard> intelCards_;
    std::optional<MaliGpu> mali_;
};

// ---- Everything live, in one place -----------------------------------------

class LiveMonitor {
public:
    LiveMonitor() : previousCpu_(readCpuTimes()), cpuVoltageInputs_(findCpuVoltageInputs()) {
        stats_.cpuPercent.assign(previousCpu_.size(), 0.0f);
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
    void updateCpuLoad() {
        std::vector<CpuTimes> now = readCpuTimes();
        if (now.size() != previousCpu_.size()) {
            stats_.cpuPercent.assign(now.size(), 0.0f);   // a core went on/offline; start over
        } else {
            for (size_t i = 0; i < now.size(); ++i) {
                const double totalDelta  = static_cast<double>(now[i].total - previousCpu_[i].total);
                const double activeDelta = static_cast<double>(now[i].active - previousCpu_[i].active);
                if (totalDelta > 0.0) stats_.cpuPercent[i] = static_cast<float>(100.0 * activeDelta / totalDelta);
            }
        }
        previousCpu_ = std::move(now);
    }

    void readSensors() {
        stats_.ram        = readRamStats();
        stats_.gpu        = gpu_.read();
        stats_.cpuFreqGhz = readCpuFreqGhz();
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
    GpuMonitor gpu_;
    LiveStats stats_;
};

// ============================================================================
// 5. UI
// ============================================================================

// ---- Settings --------------------------------------------------------------

enum class Skin { Classic, Jarv, Galactic, Gunship, Halloween };

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
};

bool operator!=(const Settings& a, const Settings& b) {
    return a.skin != b.skin || a.animations != b.animations || a.refreshMs != b.refreshMs ||
           a.runInBackground != b.runInBackground || a.limitOverlayToApp != b.limitOverlayToApp;
}

std::string settingsFilePath() {
    const char* configHome = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (configHome && *configHome) return std::string(configHome) + "/veeastats/settings.conf";
    if (home && *home)             return std::string(home) + "/.config/veeastats/settings.conf";
    return "";
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
    fprintf(file, "skin=%s\nanimations=%d\nrefresh_ms=%d\nbackground=%d\nlimit_overlay=%d\n",
            skinChoice(settings.skin).id, settings.animations ? 1 : 0, settings.refreshMs,
            settings.runInBackground ? 1 : 0, settings.limitOverlayToApp ? 1 : 0);
    fclose(file);
}

// Everything the user can change from the window. main() reacts to changes after each frame.
struct UiState {
    bool settingsOpen{false};   // the gear's menu is showing
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
const ImVec4 kErrorColor  (0.90f, 0.30f, 0.30f, 1.0f);

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

// "3.20 GHz | 1.100 V", or greyed-out "N/A" placeholders when a sensor is missing.
void drawClockAndVoltage(double clockGhz, double volts, const char* voltageName) {
    if (clockGhz > 0.0) ImGui::TextColored(kClockColor, "%.2f GHz", clockGhz);
    else                ImGui::TextDisabled("Clock: N/A");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (volts > 0.0) ImGui::TextColored(kVoltageColor, "%.3f V", volts);
    else             ImGui::TextDisabled("%s: N/A", voltageName);
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

void drawCpuSection(const StaticInfo& info, const LiveStats& live) {
    ImGui::TextColored(kCpuHeading, "Per-Core CPU Load");
    ImGui::TextWrapped("%s", info.cpuModel.c_str());
    drawClockAndVoltage(live.cpuFreqGhz, live.cpuVoltage, "VCore");
    ImGui::Spacing();

    ImGui::BeginChild("CpuCoresRegion", ImVec2(0, 200), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (size_t i = 0; i < live.cpuPercent.size(); ++i) {
        drawUsageBar(format("CPU %2zu", i).c_str(), live.cpuPercent[i], format("%.1f%%", live.cpuPercent[i]), 0.50f, 0.80f);
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

void drawGpuSection(const LiveStats& live) {
    const GpuStats& gpu = live.gpu;

    ImGui::TextColored(kGpuHeading, "GPU Metrics%s", vendorTag(gpu.vendor).c_str());
    ImGui::TextWrapped("Card: %s", gpu.name.c_str());
    drawClockAndVoltage(gpu.clockGhz, gpu.voltageV, "VDDC");
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

// A gear-shaped button. The default font has no icon characters, so the gear is
// drawn from shapes: a thick ring for the body plus rectangles for the teeth.
// Returns true when clicked. `active` keeps it highlighted while its menu is open.
bool drawGearButton(const char* id, float size, bool active) {
    constexpr int kTeeth = 8;

    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Settings");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || active) {
        draw->AddRectFilled(topLeft, ImVec2(topLeft.x + size, topLeft.y + size),
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered), ImGui::GetStyle().FrameRounding);
    }

    const ImU32 color = ImGui::GetColorU32((hovered || active) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 center(topLeft.x + size * 0.5f, topLeft.y + size * 0.5f);
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

// The whole window in the Classic skin: performance on top, system details below.
// `ui` is read and (if the user changes something) updated here.
void drawDashboard(const ImVec2& windowSize, UiState& ui, const StaticInfo& info, const LiveStats& live) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(windowSize);
    ImGui::Begin("VeeaStats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Title on the left, settings gear pinned to the right edge of the same row.
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float gearSize = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();   // centres the title on the gear's height
    ImGui::TextColored(kTitleColor, "System Performance Dashboard");
    ImGui::SameLine(rightEdge - gearSize);
    if (drawGearButton("##Settings", gearSize, ui.settingsOpen)) ui.settingsOpen = !ui.settingsOpen;
    const ImVec2 gearMin = ImGui::GetItemRectMin(), gearMax = ImGui::GetItemRectMax();

    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiTableFlags columnFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Resizable;

    if (ImGui::BeginTable("PerformanceTable", 2, columnFlags)) {
        ImGui::TableNextColumn();   // left: CPU + RAM
        drawCpuSection(info, live);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        drawRamSection(info, live);

        ImGui::TableNextColumn();   // right: GPU
        drawGpuSection(live);
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

    if (ui.settingsOpen) drawSettingsMenu(ui, gearMin, gearMax);
}

// ============================================================================
// 6. HUD SKINS (JARV, GALACTIC CONFLICT, FEDERATION GUNSHIP, HALLOWEEN)
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
    float time{0.0f};               // seconds; drives the moving parts (stays 0 when not animating)
    // The numbers as currently drawn. While animating they glide towards the
    // live values instead of jumping.
    float cpu{0.0f}, ram{0.0f}, gpu{0.0f}, vram{0.0f};
    std::vector<float> cores;
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
}

// What each of the four gauges shows.
struct GaugeReading {
    std::string label;          // e.g. "CPU"
    float percent;              // as drawn (smoothed)
    bool hasValue;
    float warnAt, critAt;       // 0..1: where it turns to the "busy" / "critical" colour
    std::string line1, line2;   // readouts under the gauge
};

std::array<GaugeReading, 4> gaugeReadings(const HudState& state, const LiveStats& live) {
    const GpuStats& gpu = live.gpu;
    const bool gpuReports = gpu.hasLoad || gpu.hasMemory || !gpu.loadNote.empty();
    return {{
        {"CPU", state.cpu, !live.cpuPercent.empty(), 0.50f, 0.80f,
         live.cpuFreqGhz > 0.0 ? format("%.2f GHz", live.cpuFreqGhz) : "CLOCK N/A",
         live.cpuVoltage > 0.0 ? format("VCORE %.3f V", live.cpuVoltage) : "VCORE N/A"},
        {"MEMORY", state.ram, live.ram.totalGb > 0.0, 0.65f, 0.85f,
         format("%.1f / %.1f GB", live.ram.usedGb, live.ram.totalGb), "SYSTEM RAM"},
        {gpu.loadIsEstimate ? "GPU (EST.)" : "GPU", state.gpu, gpu.hasLoad, 0.50f, 0.85f,
         gpu.clockGhz > 0.0 ? format("%.2f GHz", gpu.clockGhz) : "CLOCK N/A",
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
    // Title, clock and the gear button. Returns the gear's rectangle.
    Box (*header)(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state){};
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
    if (beginHudInfoTable(skin, "HudGpuTable", skin.labelColumns.graphics, {"MODEL", "VENDOR", "CLOCK", "VDDC", "LOAD", "VRAM"})) {
        skin.infoRow(state, "MODEL", gpu.name);
        skin.infoRow(state, "VENDOR", vendorName(gpu.vendor));
        skin.infoRow(state, "CLOCK", gpu.clockGhz > 0.0 ? format("%.2f GHz", gpu.clockGhz) : "N/A");
        skin.infoRow(state, "VDDC", gpu.voltageV > 0.0 ? format("%.3f V", gpu.voltageV) : "N/A");
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

    const Box gear = skin.header(draw, Box{origin, origin + ImVec2(width, kHeaderHeight)}, ui, state);

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

    if (ui.settingsOpen) drawSettingsMenu(ui, gear.min, gear.max);
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

// Places the gear button at the right end of a header row. Returns its rectangle.
Box drawHeaderGear(UiState& ui, float right, float midY) {
    const float gearSize = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(right - gearSize, midY - gearSize * 0.5f));
    if (drawGearButton("##Settings", gearSize, ui.settingsOpen)) ui.settingsOpen = !ui.settingsOpen;
    return Box{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
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
        const std::string name = format("C%02d", i);
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
// right; a glowing rule underneath. Returns the gear's rectangle.
Box drawJarvHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& jarv) {
    ImFont* display = jarv.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 2.0f;

    const ImVec2 titleExtent = textSize(display, 20.0f, "VEEASTATS");
    drawGlowText(draw, display, 20.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), kJarvCyan, "VEEASTATS");
    const ImVec2 subtitleExtent = textSize(display, 10.0f, "// SYSTEM DIAGNOSTICS");
    draw->AddText(display, 10.0f, ImVec2(box.min.x + titleExtent.x + 12.0f, midY - subtitleExtent.y * 0.5f + 2.0f),
                  withAlpha(kJarvMuted, 1.0f), "// SYSTEM DIAGNOSTICS");

    // Right-hand side, placed from the right edge inwards.
    const Box gear = drawHeaderGear(ui, box.max.x, midY);
    float right = gear.min.x - 20.0f;

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
    return gear;
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
// the gear on the right; a double rule underneath. Returns the gear's rectangle.
Box drawGalacticHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 3.0f;

    const ImVec2 titleExtent = textSize(display, 18.0f, "VEEASTATS");
    draw->AddText(display, 18.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), withAlpha(kGcWhite, 1.0f), "VEEASTATS");
    const float subtitleX = box.min.x + titleExtent.x + 14.0f;
    draw->AddRectFilled(ImVec2(subtitleX, midY - 2.0f), ImVec2(subtitleX + 4.0f, midY + 2.0f), withAlpha(kGcRed, 1.0f));
    const ImVec2 subtitleExtent = textSize(display, 9.0f, "TACTICAL READOUT");
    draw->AddText(display, 9.0f, ImVec2(subtitleX + 10.0f, midY - subtitleExtent.y * 0.5f), withAlpha(kGcGold, 1.0f), "TACTICAL READOUT");

    const Box gear = drawHeaderGear(ui, box.max.x, midY);
    float right = gear.min.x - 20.0f;

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
    return gear;
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

        drawOutlinedText(panel, state.displayFont, 8.0f, a + ImVec2(6.0f, 6.0f), kFgWhite, format("C%02d", i));
        const std::string text = format("%.0f%%", value);
        const ImVec2 extent = textSize(state.textFont, 16.0f, text);
        drawOutlinedText(panel, state.textFont, 16.0f, ImVec2(b.x - extent.x - 6.0f, b.y - extent.y - 4.0f), kFgWhite, text);
    }
    ImGui::Dummy(ImVec2(room.x, (cellHeight + kGap) * static_cast<float>(rows) - kGap));
    ImGui::EndChild();
}

// Magenta pixel title; a "LIVE" legend with a blinking blue light, the clock and
// the gear on the right; a pale rule with notches underneath. Returns the gear's rectangle.
Box drawGunshipHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    ImFont* display = state.displayFont;
    const float midY = snapToPixel((box.min.y + box.max.y) * 0.5f) - 4.0f;

    drawOutlinedText(draw, display, 16.0f, ImVec2(box.min.x, midY - 8.0f), kFgMagenta, "VEEASTATS", kPx);
    drawOutlinedText(draw, display, 8.0f, ImVec2(box.min.x + textSize(display, 16.0f, "VEEASTATS").x + 14.0f, midY - 2.0f), kFgYellow,
                  "SYSTEM STATUS");

    const Box gear = drawHeaderGear(ui, box.max.x, midY);
    float right = gear.min.x - 18.0f;

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
    return gear;
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

        drawHalloweenText(panel, state.textFont, 12.0f, a + ImVec2(6.0f, 3.0f), kHwMist, format("C%02d", i), 1.0f);
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
// along the bottom. Returns the gear's rectangle.
Box drawHalloweenHeader(ImDrawList* draw, const Box& box, UiState& ui, const HudState& state) {
    const float midY = std::floor((box.min.y + box.max.y) * 0.5f) - 5.0f;
    const ImVec2 titleExtent = textSize(state.displayFont, 34.0f, "VeeaStats");
    drawHalloweenText(draw, state.displayFont, 34.0f, ImVec2(box.min.x, midY - titleExtent.y * 0.5f), kHwOrange, "VeeaStats", 2.5f);
    const ImVec2 subtitleExtent = textSize(state.textFont, 13.0f, "Haunted Hardware");
    drawHalloweenText(draw, state.textFont, 13.0f, ImVec2(box.min.x + titleExtent.x + 14.0f, midY - subtitleExtent.y * 0.5f + 3.0f), kHwMist,
                      "Haunted Hardware", 1.0f);

    const Box gear = drawHeaderGear(ui, box.max.x, midY);
    float right = gear.min.x - 18.0f;

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
    return gear;
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
    return fonts;
}

// Every HUD skin, ready to draw (see section 6).
struct HudSkins {
    HudSkin jarv, galactic, gunship, halloween;

    const HudSkin& forSkin(Skin skin) const {
        switch (skin) {
            case Skin::Galactic:  return galactic;
            case Skin::Gunship:   return gunship;
            case Skin::Halloween: return halloween;
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

