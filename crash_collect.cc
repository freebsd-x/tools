// xcrash-collect — group and summarize FreeBSD-X crash spool files.
//
// The crash handler (core/xserver/src/crash_handler.cc) writes one file per
// crash to ~/.freebsdx/crashes/<proc>-<pid>.crash:
//
//     === FreeBSD-X crash: SIGSEGV @ 0x0000... ===
//     0x.... <symbol+0xNN> at /path/to/binary
//     ...
//
// Repeated crashes of the same bug produce many near-identical files. This tool
// dedups them by a stack signature (the top frames' symbols, minus the handler's
// own frames and per-run addresses) so a recurring crash shows as one entry with
// a count and last-seen, instead of N noisy files.
//
// Usage:
//   xcrash-collect [dir]        summarize (default ~/.freebsdx/crashes)
//   xcrash-collect --json [dir] machine-readable output (for the crash UI)
//   xcrash-collect --clear [dir]delete all .crash files after summarizing
//   xcrash-collect --top N      frames used for the signature (default 5)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Crash {
    std::string app;
    std::string signal   = "?";
    std::string fault;              // fault address, if any
    std::time_t when     = 0;       // file mtime
    std::vector<std::string> frames;    // symbol per frame (handler frames dropped)
    std::string sig_key;                // dedup signature
};

// Frames belonging to the crash handler / signal trampoline — never part of the
// bug's signature.
bool isHandlerFrame(const std::string& sym) {
    static const char* skip[] = {"crashHandler", "dumpTo", "installCrashHandler",
                                 "sigcode", "__vdso_sigcode",
                                 "abort", "raise", "_thr_kill", "pthread_kill"};
    for (const char* s : skip)
        if (sym.find(s) != std::string::npos) return true;
    return false;
}

// Pull the symbol out of a backtrace line: "0x.. <SYM+0xNN> at /path" -> "SYM".
// Returns "" if the line carries no <...> symbol.
std::string frameSymbol(const std::string& line) {
    auto lt = line.find('<');
    auto gt = line.find('>', lt == std::string::npos ? 0 : lt);
    if (lt == std::string::npos || gt == std::string::npos) return {};
    std::string sym = line.substr(lt + 1, gt - lt - 1);
    if (auto plus = sym.rfind('+'); plus != std::string::npos) sym.erase(plus);  // drop +0xNN
    return sym;
}

// FNV-1a over the joined signature frames — stable across runs.
std::string hashKey(const std::vector<std::string>& frames, int top) {
    unsigned long long h = 1469598103934665603ULL;
    int n = 0;
    for (const auto& f : frames) {
        if (n++ >= top) break;
        for (char c : f) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
        h ^= '|'; h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

bool parse(const fs::path& p, int top, Crash& out) {
    std::ifstream in(p);
    if (!in) return false;

    out.app = p.stem().string();                 // "<proc>-<pid>"
    if (auto dash = out.app.rfind('-'); dash != std::string::npos)
        out.app.erase(dash);                     // strip -<pid>
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (!ec) {
        // Convert file_time to time_t (portable-enough approximation).
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        out.when = std::chrono::system_clock::to_time_t(sctp);
    }

    std::string line;
    bool seen_trampoline = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.find("=== FreeBSD-X crash:") != std::string::npos) {
            // "=== FreeBSD-X crash: SIGSEGV @ 0x... ==="
            auto c = line.find("crash:");
            std::string rest = line.substr(c + 6);
            char sig[16] = {0}, addr[32] = {0};
            std::sscanf(rest.c_str(), " %15s @ %31s", sig, addr);
            out.signal = sig[0] ? sig : "?";
            if (addr[0] && std::strcmp(addr, "===") != 0) out.fault = addr;
            continue;
        }
        std::string sym = frameSymbol(line);
        if (sym.empty()) sym = "??";
        // Drop leading handler/trampoline frames from the signature; keep the
        // first real fault frame onward.
        if (!seen_trampoline) {
            if (isHandlerFrame(sym)) continue;
            seen_trampoline = true;
        }
        out.frames.push_back(sym);
    }
    out.sig_key = hashKey(out.frames, top);
    return true;
}

std::string humanTime(std::time_t t) {
    if (t == 0) return "unknown";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

struct Group {
    std::string signal;
    std::vector<std::string> frames;  // representative (from newest crash)
    std::map<std::string, int> apps;  // app -> count
    int count = 0;
    std::time_t last = 0;
};

}  // namespace

int main(int argc, char** argv) {
    bool json = false, clear = false;
    int top = 5;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--clear") clear = true;
        else if (a == "--top" && i + 1 < argc) top = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] != '-') dir = a;
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : ".") + "/.freebsdx/crashes";
    }

    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".crash") files.push_back(e.path());
    }

    std::map<std::string, Group> groups;
    for (const auto& f : files) {
        Crash c;
        if (!parse(f, top, c)) continue;
        Group& g = groups[c.sig_key];
        g.count++;
        g.apps[c.app]++;
        if (c.when >= g.last) { g.last = c.when; g.signal = c.signal; g.frames = c.frames; }
    }

    // Sort groups by count desc, then most-recent desc.
    std::vector<std::pair<std::string, Group>> sorted(groups.begin(), groups.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second.count != b.second.count) return a.second.count > b.second.count;
        return a.second.last > b.second.last;
    });

    if (json) {
        std::printf("[");
        for (size_t i = 0; i < sorted.size(); ++i) {
            const auto& [key, g] = sorted[i];
            std::printf("%s\n  {\"sig\":\"%s\",\"signal\":\"%s\",\"count\":%d,\"last\":%lld,\"apps\":[",
                        i ? "," : "", key.c_str(), g.signal.c_str(), g.count,
                        static_cast<long long>(g.last));
            bool first = true;
            for (const auto& [app, n] : g.apps) {
                std::printf("%s{\"app\":\"%s\",\"count\":%d}", first ? "" : ",", app.c_str(), n);
                first = false;
            }
            std::printf("],\"frames\":[");
            for (size_t k = 0; k < g.frames.size() && k < static_cast<size_t>(top); ++k)
                std::printf("%s\"%s\"", k ? "," : "", g.frames[k].c_str());
            std::printf("]}");
        }
        std::printf("\n]\n");
    } else {
        std::printf("%zu crash file(s) in %s -> %zu unique signature(s)\n\n",
                    files.size(), dir.c_str(), sorted.size());
        for (const auto& [key, g] : sorted) {
            std::string applist;
            for (const auto& [app, n] : g.apps) {
                if (!applist.empty()) applist += ", ";
                applist += app + (n > 1 ? " x" + std::to_string(n) : "");
            }
            std::printf("[%dx] %s  in %s   last: %s   (sig %s)\n",
                        g.count, g.signal.c_str(), applist.c_str(),
                        humanTime(g.last).c_str(), key.c_str());
            for (size_t k = 0; k < g.frames.size() && k < static_cast<size_t>(top); ++k)
                std::printf("     %s\n", g.frames[k].c_str());
            std::printf("\n");
        }
    }

    if (clear) {
        int removed = 0;
        for (const auto& f : files) if (fs::remove(f, ec)) ++removed;
        std::fprintf(stderr, "cleared %d crash file(s)\n", removed);
    }
    return 0;
}
