// reclockbar — binary helper for the Omarchy bar (instead of .sh scripts).
//
//   reclockbar watch [interval_ms] [window]   — JSON stream (1 line/sample),
//                                               history+stats computed here
//   reclockbar once                           — single sample (no hist/stats)
//   reclockbar gpu-temp                       — {"temp","src","ps"} (drop-in
//                                               bar-gpu-temp.sh, gpu.qml)
//   reclockbar fan <50..100|auto|toggle>      — action: fan FLOOR
//                                               (+ persistence in the user profile)
//   reclockbar fan-restore                    — restore the floor from the profile
//                                               (QML runs it once at bar startup)
//   reclockbar dgpu <auto|on|off>             — action: dgpu-override flag
//
// Patterns carried over 1:1 from the sh version (2026-09-12):
//   - fan-override: flag /run/reclocked/fan-override. Daemon v5.16: content =
//     FLOOR % (50-100) — the curve may raise RPM, never drops below;
//     empty content = old hold (reclockctl fan-off creates an empty file — OK).
//   - race with the daemon tick (1 s): double RPM write before/after the flag write.
//   - dgpu-override: content "on\n"/"off\n", no file = auto (reclocked.cpp
//     dgpu_override()); "on" clears the dgpu-dead gate (one attempt, pattern
//     reclockctl dgpu-on; reclocked.cpp c. 2952).
//   - daemon dead at fan auto → fanN_manual=0 (SMC auto, don't leave
//     frozen RPM).
//
// Stats (watch): sliding window (default 10 s × 1 s) per sensor
// (CPU/GPU): avg/min/max, trend = least-squares slope [°C/s], pred60 =
// last + slope·60. Zero external dependencies. Read paths rootless
// (sysfs/status/flags 0644); actions run via `sudo -n reclockbar`.

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>

static const char* SMC      = "/sys/devices/platform/applesmc.768";
static const char* RUNDIR   = "/run/reclocked";
static const char* STATUS   = "/run/reclocked/status";
static const char* FAN_FLAG = "/run/reclocked/fan-override";
static const char* DGPU_FLAG = "/run/reclocked/dgpu-override";
static const char* DGPU_DEAD = "/run/reclocked/dgpu-dead";

// ---------------------------------------------------------------- files
static bool read_file(const char* path, std::string& out)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buf[256];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static int read_int_path(const char* path)
{
    // Hot path (1 Hz × many files): raw open/read — no fopen/fread or string
    // allocations; strtol skips whitespace itself.
    char buf[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)strtol(buf, nullptr, 10);
}

// ---------------------------------------------------------------- status JSON
// The daemon writes flat JSON (strings/numbers/bools). A key extractor is enough
// (the json_get pattern from reclockctl) — no parser/dependencies.
static std::string json_get(const std::string& j, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p >= j.size()) return "";
    if (j[p] == '"') {                       // string
        size_t e = j.find('"', p + 1);
        if (e == std::string::npos) return "";
        return j.substr(p + 1, e - p - 1);
    }
    size_t e = p;                            // number or true/false
    while (e < j.size() && (isdigit((unsigned char)j[e]) || j[e] == '-' ||
                            j[e] == '.' || j[e] == 'e' || j[e] == 'E' ||
                            j[e] == '+'))
        e++;
    if (j[p] == 't' || j[p] == 'f') {        // true/false — word token
        size_t q = p;
        while (q < j.size() && isalpha((unsigned char)j[q])) q++;
        return j.substr(p, q - p);
    }
    if (e == p) return "";
    return j.substr(p, e - p);
}

static double json_num(const std::string& j, const char* key, double dflt)
{
    std::string v = json_get(j, key);
    if (v.empty()) return dflt;
    return atof(v.c_str());
}

// ---------------------------------------------------------------- hwmon
// hwmon numbers change between boots and after S3 (zombie coretemp,
// report 114) → cached path, re-resolve on failure and periodically (30 samples =
// ~30 s), because a stale node may stay readable (stale).
struct HwmonCache {
    std::string path;      // /sys/class/hwmon/hwmonN/temp1_input
    int age = 0;           // samples since the last resolve
    bool valid = false;
};
static HwmonCache g_core, g_nv;   // coretemp (Package), nouveau

static bool resolve_hwmon(const char* name, HwmonCache& c)
{
    DIR* d = opendir("/sys/class/hwmon");
    if (!d) return false;
    bool ok = false;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        char p[300];   // d_name up to 255 + "/sys/class/hwmon/" + "/name"
        snprintf(p, sizeof(p), "/sys/class/hwmon/%s/name", e->d_name);
        std::string nm;
        if (!read_file(p, nm) || trim(nm) != name) continue;
        c.path = std::string("/sys/class/hwmon/") + e->d_name + "/temp1_input";
        c.valid = true;
        c.age = 0;
        ok = true;
        break;
    }
    closedir(d);
    return ok;
}

// Zwraca °C lub -1 (hit = 1 read; miss = 1 scan).
static int hwmon_temp(const char* name, HwmonCache& c)
{
    if (c.valid) {
        if (++c.age < 30) {
            int t = read_int_path(c.path.c_str());
            if (t >= 0) return t / 1000;
        }
        c.valid = false;                 // fail OR age≥30 → re-resolve
    }
    if (!resolve_hwmon(name, c)) return -1;
    int t = read_int_path(c.path.c_str());
    return t >= 0 ? t / 1000 : -1;
}

// ---------------------------------------------------------------- fans
struct FanSnapshot {
    int out1 = 0, out2 = 0, min1 = 0, min2 = 0, max1 = 0, max2 = 0;
    bool ok = false;
};

// fanN_min/max are constant for the SMC → cached (30 samples), output/input fresh per tick.
struct FanRange {
    int min1 = -1, min2 = -1, max1 = -1, max2 = -1;
    int age = 0;
    bool ok = false;
};
static FanRange g_fan_range;

static FanSnapshot fans_read(bool cache = false)
{
    FanSnapshot f;
    char path[256];
    auto rd = [&](const char* leaf) -> int {
        snprintf(path, sizeof(path), "%s/%s", SMC, leaf);
        return read_int_path(path);
    };
    f.out1 = rd("fan1_output"); f.out2 = rd("fan2_output");
    if (cache && g_fan_range.ok && ++g_fan_range.age < 30) {
        f.min1 = g_fan_range.min1; f.min2 = g_fan_range.min2;
        f.max1 = g_fan_range.max1; f.max2 = g_fan_range.max2;
    } else {
        f.min1 = rd("fan1_min");    f.min2 = rd("fan2_min");
        f.max1 = rd("fan1_max");    f.max2 = rd("fan2_max");
        if (f.max1 > 0 && f.max2 > 0) {
            g_fan_range.min1 = f.min1; g_fan_range.min2 = f.min2;
            g_fan_range.max1 = f.max1; g_fan_range.max2 = f.max2;
            g_fan_range.ok = true;
            g_fan_range.age = 0;
        }
    }
    f.ok = f.max1 > 0 && f.max2 > 0;
    return f;
}

static bool daemon_alive()
{
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool alive = false;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[280];   // d_name up to 255 + "/proc/" + "/comm"
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        std::string c;
        if (read_file(path, c) && trim(c) == "reclocked") { alive = true; break; }
    }
    closedir(d);
    return alive;
}

static void write_file_int(const char* path, int v)
{
    FILE* f = fopen(path, "w");
    if (f) { fprintf(f, "%d", v); fclose(f); }
}

static void fan_manual(int on)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/fan1_manual", SMC); write_file_int(p, on);
    snprintf(p, sizeof(p), "%s/fan2_manual", SMC); write_file_int(p, on);
}

static void fan_output(int v1, int v2)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/fan1_output", SMC); write_file_int(p, v1);
    snprintf(p, sizeof(p), "%s/fan2_output", SMC); write_file_int(p, v2);
}

// ---------------------------------------------------------------- state in profile
// v5.16: X% = FLOOR (lower bound — daemon v5.16: the curve may raise,
// never drops below; empty flag = old hold). The flag lives in tmpfs
// (/run) → the floor state persists in the user profile; `fan-restore` (QML
// runs it once at bar startup) restores the flag + seed RPM after reboot.
// Actions run under sudo → profile file is chown'd back to SUDO_USER.
// State in the USER's PROFILE, not root's: sudo resets HOME=/root — we take
// home from SUDO_USER (getpwnam). Without sudo (command line) the plain user's HOME.
static std::string user_home()
{
    const char* su = getenv("SUDO_USER");
    if (su && *su) {
        struct passwd* pw = getpwnam(su);
        if (pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    }
    return getenv("HOME") ? getenv("HOME") : "/root";
}
static std::string state_path()
{
    const char* xdg = getenv("XDG_STATE_HOME");
    std::string base = (xdg && *xdg) ? xdg : user_home() + "/.local/state";
    return base + "/omarchy/reclockbar-fans";
}

static void state_write(const std::string& content)
{
    std::string p = state_path();
    std::string base = p.substr(0, p.find_last_of('/'));
    mkdir(base.c_str(), 0755);
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fputs(content.c_str(), f);
    fclose(f);
    const char* su = getenv("SUDO_USER");
    if (su) {
        struct passwd* pw = getpwnam(su);
        if (pw) chown(p.c_str(), pw->pw_uid, pw->pw_gid);
    }
}

static void state_clear() { unlink(state_path().c_str()); }

// ---------------------------------------------------------------- stats
// Sliding window + least-squares. Internal buffer (watch pushes; once doesn't use it).
struct Series {
    std::deque<double> v;
    double avg = 0, mn = 0, mx = 0, slope = 0, pred = 0;
    bool has = false;

    void push(double x, size_t win)
    {
        v.push_back(x);
        while (v.size() > win) v.pop_front();
        compute();
    }
    void compute()
    {
        size_t n = v.size();
        has = n >= 2;
        if (!n) return;
        mn = mx = v[0];
        double sum = 0;
        for (double x : v) {
            if (x < mn) mn = x;
            if (x > mx) mx = x;
            sum += x;
        }
        avg = sum / (double)n;
        // least squares: slope = Σ(t−t̄)(y−ȳ) / Σ(t−t̄)², t = sample index
        if (n >= 2) {
            double tb = (double)(n - 1) / 2.0, sxy = 0, sxx = 0;
            for (size_t i = 0; i < n; i++) {
                double dt = (double)i - tb, dy = v[i] - avg;
                sxy += dt * dy;
                sxx += dt * dt;
            }
            slope = sxx > 0 ? sxy / sxx : 0;
            pred = v[n - 1] + slope * 60.0;
            if (pred < 0) pred = 0;
            if (pred > 110) pred = 110;
        } else {
            slope = 0;
            pred = v[0];
        }
    }
};

// ---------------------------------------------------------------- emit
static void escape_json(std::string& out, const std::string& in)
{
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
}

// One sample → 1 JSON line. cs/gs != nullptr only in watch (buffer + stats).
static std::string sample(const FanSnapshot& f, Series* cs, Series* gs, size_t win)
{
    std::string st;
    read_file(STATUS, st);
    std::string pow  = json_get(st, "dgpu_power");
    std::string ovr  = json_get(st, "override");
    std::string la   = json_get(st, "last_action");
    std::string err  = json_get(st, "last_error");
    std::string ver  = json_get(st, "version");
    std::string topo = json_get(st, "topology");
    std::string curve = json_get(st, "fan_curve");
    std::string dead = json_get(st, "dgpu_dead");
    std::string deadr = json_get(st, "dgpu_dead_reason");
    double case_avg = json_num(st, "fan_case_avg", -1.0);

    int cpu = hwmon_temp("coretemp", g_core);
    int gpu = (pow == "on") ? hwmon_temp("nouveau", g_nv) : -1;
    if (cpu < 0 && gpu >= 0) cpu = gpu;   // coretemp down? (fallback pattern)

    // flags (existence = state; content = % for fan-override)
    std::string fl;
    int fpct = 0;
    bool fov = read_file(FAN_FLAG, fl);
    if (fov) {
        fpct = atoi(trim(fl).c_str());
        if (fpct < 50 || fpct > 100) fpct = 100;   // empty content (reclockctl fan-off) = 100%
    }
    bool gov = read_file(DGPU_FLAG, fl);
    std::string govr = gov ? trim(fl) : "";

    if (cs && cpu >= 0) cs->push(cpu, win);
    if (gs && gpu >= 0) gs->push(gpu, win);

    std::string out = "{";
    auto S = [&](const char* k, const std::string& v) {
        out += "\""; out += k; out += "\":\"";
        escape_json(out, v); out += "\",";
    };
    auto N = [&](const char* k, long long v) {
        char b[32]; snprintf(b, sizeof(b), "%lld", v);
        out += "\""; out += k; out += "\":"; out += b; out += ",";
    };
    auto F = [&](const char* k, double v, int dec) {
        char b[32]; snprintf(b, sizeof(b), "%.*f", dec, v);
        out += "\""; out += k; out += "\":"; out += b; out += ",";
    };

    S("v", ver); S("topo", topo); S("pow", pow);
    S("st", json_get(st, "dgpu_state")); S("tgt", json_get(st, "target"));
    S("ovr", govr); S("curve", curve); S("la", la); S("err", err);
    N("dead", (long long)(dead == "true" ? 1 : 0));
    if (dead == "true") S("deadr", deadr);
    N("freq", (long long)json_num(st, "igpu_freq_mhz", 0));
    N("r1", (long long)json_num(st, "fan_rpm1", 0));
    N("r2", (long long)json_num(st, "fan_rpm2", 0));
    N("o1", f.out1); N("o2", f.out2);
    N("n1", f.min1); N("n2", f.min2);
    N("m1", f.max1); N("m2", f.max2);
    N("fov", fov ? 1 : 0);
    N("fpct", fpct);
    F("case", case_avg, 1);
    N("cpu", cpu);
    if (gpu >= 0) N("gpu", gpu); else out += "\"gpu\":null,";

    // window history — watch only (QML sparkline draws from these arrays)
    if (cs) {
        out += "\"ch\":[";
        for (size_t i = 0; i < cs->v.size(); i++) {
            if (i) out += ",";
            char b[16]; snprintf(b, sizeof(b), "%d", (int)cs->v[i]); out += b;
        }
        out += "],\"gh\":[";
        for (size_t i = 0; i < gs->v.size(); i++) {
            if (i) out += ",";
            char b[16]; snprintf(b, sizeof(b), "%d", (int)gs->v[i]); out += b;
        }
        out += "],";
    }

    // window stats: avg/min/max/slope/pred — emit when ≥2 samples
    if (cs && cs->has) {
        char b[96];
        snprintf(b, sizeof(b),
                 "\"cpu_avg\":%.1f,\"cpu_min\":%.0f,\"cpu_max\":%.0f,"
                 "\"cpu_slope\":%.2f,\"cpu_pred\":%.1f,",
                 cs->avg, cs->mn, cs->mx, cs->slope, cs->pred);
        out += b;
    }
    if (gs && gs->has) {
        char b[96];
        snprintf(b, sizeof(b),
                 "\"gpu_avg\":%.1f,\"gpu_min\":%.0f,\"gpu_max\":%.0f,"
                 "\"gpu_slope\":%.2f,\"gpu_pred\":%.1f,",
                 gs->avg, gs->mn, gs->mx, gs->slope, gs->pred);
        out += b;
    }

    N("ts", (long long)time(nullptr));
    if (!out.empty() && out.back() == ',') out.pop_back();
    out += "}";
    return out;
}

// ---------------------------------------------------------------- actions
static int cmd_fan(const char* arg)
{
    FanSnapshot f = fans_read();
    if (!f.ok) {
        fprintf(stderr, "reclockbar fan: missing/nonsense fanN_max\n");
        return 1;
    }
    std::string pct_s = arg;
    if (pct_s == "toggle") {
        std::string fl;
        pct_s = read_file(FAN_FLAG, fl) ? "auto" : "100";
    }
    if (pct_s == "auto") {
        unlink(FAN_FLAG);
        state_clear();                        // v5.16: do NOT restore after reboot
        if (!daemon_alive()) fan_manual(0);   // nobody will take over → SMC auto
        printf("{\"state\":\"auto\"}\n");
        return 0;
    }
    int pct = atoi(pct_s.c_str());
    if (pct < 50 || pct > 100) {
        fprintf(stderr, "reclockbar fan: percentage out of range (50-100): %s\n", pct_s.c_str());
        return 1;
    }
    int c1 = (f.max1 * pct + 50) / 100;      // round half-up
    int c2 = (f.max2 * pct + 50) / 100;
    if (f.min1 > 0 && c1 < f.min1) c1 = f.min1;
    if (f.min2 > 0 && c2 < f.min2) c2 = f.min2;
    fan_manual(1);
    fan_output(c1, c2);
    FILE* fl = fopen(FAN_FLAG, "w");         // content = floor % (daemon v5.16 reads
    if (fl) { fprintf(fl, "%d\n", pct); fclose(fl); }   // content: curve ≥ floor)
    fan_output(c1, c2);                      // second write — closes the race with the tick
    state_write(std::to_string(pct) + "\n"); // persistence in the user profile
    printf("{\"state\":\"floor\",\"pct\":%d}\n", pct);
    return 0;
}

// fan-restore — restore the floor from the user profile after reboot (QML runs it once
// at bar startup, under sudo): seed manual+RPM+flag; daemon v5.16 takes over
// the floor on the next tick (curve ≥ floor).
static int cmd_fan_restore()
{
    std::string s;
    if (!read_file(state_path().c_str(), s)) {
        printf("{\"state\":\"auto\"}\n");    // no saved state → do nothing
        return 0;
    }
    int pct = atoi(trim(s).c_str());
    if (pct < 50 || pct > 100) {
        state_clear();
        printf("{\"state\":\"auto\"}\n");
        return 0;
    }
    FanSnapshot f = fans_read();
    if (!f.ok) {
        fprintf(stderr, "reclockbar fan-restore: missing/nonsense fanN_max\n");
        return 1;
    }
    int c1 = (f.max1 * pct + 50) / 100;
    int c2 = (f.max2 * pct + 50) / 100;
    if (f.min1 > 0 && c1 < f.min1) c1 = f.min1;
    if (f.min2 > 0 && c2 < f.min2) c2 = f.min2;
    fan_manual(1);
    fan_output(c1, c2);
    FILE* fl = fopen(FAN_FLAG, "w");
    if (fl) { fprintf(fl, "%d\n", pct); fclose(fl); }
    fan_output(c1, c2);
    printf("{\"state\":\"floor\",\"pct\":%d}\n", pct);
    return 0;
}

static int cmd_dgpu(const char* arg)
{
    mkdir(RUNDIR, 0755);                     // the daemon usually created it; defensively
    std::string a = arg;
    if (a == "auto") {
        unlink(DGPU_FLAG);
        printf("{\"state\":\"auto\"}\n");
    } else if (a == "on") {
        unlink(DGPU_DEAD);                   // explicit user decision = one attempt (v5.15)
        FILE* f = fopen(DGPU_FLAG, "w");
        if (!f) { fprintf(stderr, "reclockbar dgpu: cannot write the flag\n"); return 1; }
        fprintf(f, "on\n");
        fclose(f);
        printf("{\"state\":\"on\"}\n");
    } else if (a == "off") {
        FILE* f = fopen(DGPU_FLAG, "w");
        if (!f) { fprintf(stderr, "reclockbar dgpu: cannot write the flag\n"); return 1; }
        fprintf(f, "off\n");
        fclose(f);
        printf("{\"state\":\"off\"}\n");
    } else {
        fprintf(stderr, "reclockbar dgpu: use auto|on|off\n");
        return 1;
    }
    return 0;
}

static int cmd_gpu_temp()
{
    // Drop-in bar-gpu-temp.sh: {"temp":"57000","src":"CPU","ps":""}.
    // ps: pstate hex only pre-v5.4 (daemon writes tier — v5.15 always) → ""
    // always; gpu.qml maps the tier itself, sudo/debugfs not needed.
    std::string st;
    read_file(STATUS, st);
    std::string pow = json_get(st, "dgpu_power");
    if (pow == "on") {
        int g = hwmon_temp("nouveau", g_nv);
        if (g >= 0) {
            char b[16]; snprintf(b, sizeof(b), "%d000", g);
            printf("{\"temp\":\"%s\",\"src\":\"dGPU\",\"ps\":\"\"}\n", b);
            return 0;
        }
    }
    int c = hwmon_temp("coretemp", g_core);
    if (c < 0) { printf("{\"temp\":\"\",\"src\":\"\",\"ps\":\"\"}\n"); return 0; }
    char b[16]; snprintf(b, sizeof(b), "%d000", c);
    printf("{\"temp\":\"%s\",\"src\":\"CPU\",\"ps\":\"\"}\n", b);
    return 0;
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "once";

    if (strcmp(mode, "gpu-temp") == 0) return cmd_gpu_temp();
    if (strcmp(mode, "fan") == 0) return cmd_fan(argc > 2 ? argv[2] : "toggle");
    if (strcmp(mode, "fan-restore") == 0) return cmd_fan_restore();
    if (strcmp(mode, "dgpu") == 0) return cmd_dgpu(argc > 2 ? argv[2] : "auto");
    if (strcmp(mode, "once") == 0) {
        FanSnapshot f = fans_read();
        printf("%s\n", sample(f, nullptr, nullptr, 0).c_str());
        return 0;
    }
    if (strcmp(mode, "watch") == 0) {
        long ms = argc > 2 ? atol(argv[2]) : 1000;
        size_t win = argc > 3 ? (size_t)atoi(argv[3]) : 10;
        if (ms < 100) ms = 100;
        if (win < 2) win = 2;
        Series cs, gs;
        // Cadence with clock_nanosleep(ABSTIME): no drift and catch-up of bursts
        // (sleep-per-iteration accumulates latency → uneven sample rhythm).
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        const long sec = ms / 1000, nsec = (ms % 1000) * 1000000L;
        while (true) {
            FanSnapshot f = fans_read(true);
            printf("%s\n", sample(f, &cs, &gs, win).c_str());
            fflush(stdout);
            next.tv_sec += sec;
            next.tv_nsec += nsec;
            if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
            if (getppid() == 1) break;           // parent (quickshell) died → exit
        }
        return 0;
    }
    fprintf(stderr, "reclockbar: mode '%s' — use watch|once|gpu-temp|fan|dgpu\n", mode);
    return 2;
}