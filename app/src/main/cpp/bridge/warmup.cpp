// JIT warm-up from a recorded block profile.
//
// Compiling guest code costs ~3x more than running it while the keyboard first appears. The
// addresses of compiled blocks are saved to the app's data directory, and on the next start the
// background preload thread compiles them ahead of time. Its warm JIT is then inherited by the GL
// thread through the guest thread pool (cpu.cpp).
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "cpu.h"
#include "nb.h"

namespace nb {
namespace {

constexpr u32 kMagic = 0x3150424e;  // "NBP1"

std::string profile_path() {
    if (const char* d = getenv("NB_PROFILE_DIR")) return std::string(d) + "/ninbridge-blocks.bin";
    // The package name is our process name; its private data directory is writable.
    char cmd[256] = {};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    ssize_t n = read(fd, cmd, sizeof cmd - 1);
    close(fd);
    if (n <= 0 || !strchr(cmd, '.')) return {};
    std::string pkg(cmd);
    if (auto colon = pkg.find(':'); colon != std::string::npos) pkg.resize(colon);
    std::string dir = "/data/data/" + pkg + "/files";
    if (access(dir.c_str(), W_OK) != 0) return {};
    return dir + "/ninbridge-blocks.bin";
}

double now_s() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + t.tv_nsec / 1e9;
}

Cpu* main_cpu() {
    GuestThread& t = cur_thread();
    return t.cpus.empty() ? nullptr : t.cpus[0].get();
}

void save_profile(Cpu& cpu) {
    std::string path = profile_path();
    if (path.empty()) return;
    std::vector<u32> pcs = cpu.jit->CompiledPCs();
    if (pcs.empty()) return;
    cpu.thread->compiled_hint = pcs.size();
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 header[4] = {kMagic, guest_image_id(), u32(pcs.size()), 0};
    bool ok = fwrite(header, sizeof header, 1, f) == 1 && fwrite(pcs.data(), 4, pcs.size(), f) == pcs.size();
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp.c_str(), path.c_str()) == 0) {
        NB_LOGI("warmup: saved %zu compiled blocks", pcs.size());
    } else {
        unlink(tmp.c_str());
    }
}

}  // namespace

void warmup_precompile() {
    Cpu* cpu = main_cpu();
    std::string path = profile_path();
    if (!cpu || path.empty()) return;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        NB_LOGI("warmup: no block profile yet");
        return;
    }
    u32 header[4];
    std::vector<u32> pcs;
    if (fread(header, sizeof header, 1, f) == 1 && header[0] == kMagic && header[1] == guest_image_id() &&
        header[2] < (1u << 22)) {
        pcs.resize(header[2]);
        if (fread(pcs.data(), 4, pcs.size(), f) != pcs.size()) pcs.clear();
    }
    fclose(f);
    double t0 = now_s();
    size_t done = 0;
    for (u32 pc : pcs) {
        if ((pc & 3) || !guest_is_code(pc)) continue;
        cpu->jit->Precompile(pc);
        ++done;
    }
    cpu->thread->compiled_hint = done;
    NB_LOGI("warmup: precompiled %zu blocks in %.0f ms", done, (now_s() - t0) * 1e3);
}

void warmup_note_frame() {
    static double first = 0, last_save = 0;
    static size_t saved_count = 0;
    double t = now_s();
    if (first == 0) first = t;
    // Save once the keyboard has settled, again after some typing, then whenever it grows a lot.
    static const double first_save = getenv("NB_WARMUP_FIRST_SAVE_S") ? atof(getenv("NB_WARMUP_FIRST_SAVE_S")) : 20;
    double since = t - first;
    bool due = (saved_count == 0 && since > first_save) || (last_save < first + 120 && since > 120) ||
               (since > 120 && t - last_save > 600);
    if (!due) return;
    Cpu* cpu = main_cpu();
    if (!cpu) return;
    size_t n = cpu->jit->CompiledPCs().size();
    last_save = t;
    if (saved_count && n < saved_count + saved_count / 20) return;
    save_profile(*cpu);
    saved_count = n;
}

}  // namespace nb
