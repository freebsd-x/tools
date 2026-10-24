#include "shared/logger.h"
#include "xshell/app_discovery.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <iomanip>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace fbx::xshell;

static void print_usage() {
    std::cout << "FreeBSD-X Package Manager (xpkg) v1.3\n";
    std::cout << "Usage: xpkg [command] [args]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  list                       List all installed .xapp bundles\n";
    std::cout << "  install <path>             Install a .xapp directory or .xpkg archive\n";
    std::cout << "  uninstall <id>             Uninstall an app by identifier\n";
    std::cout << "  pack <dir> [out]           Pack a .xapp directory into a .xpkg archive\n";
    std::cout << "  info <id|path>             Show details for an installed app or .xpkg\n";
    std::cout << "  search <query>             Search installed apps by name or identifier\n";
    std::cout << "  update                     Download the remote package catalog\n";
    std::cout << "  remote-list                List packages available in the remote catalog\n";
    std::cout << "  remote-search <query>      Search the remote catalog by name or description\n";
    std::cout << "  install-remote <id>        Download and install a package from the catalog\n";
    std::cout << "\nPackage Signing:\n";
    std::cout << "  sign <pkg> <secret.key>    Sign a .xpkg file with minisign (creates .minisig)\n";
    std::cout << "  verify <pkg>               Verify a .xpkg signature against the trust store\n";
    std::cout << "  trust <pubkey>             Add a public key to the trust store\n";
    std::cout << "  untrust <fingerprint>      Remove a key from the trust store by fingerprint\n";
    std::cout << "  signing-policy [on|off]    Show or set the require-signed policy\n";
}

// Minimal JSON value extractor (no dependencies).
static std::string json_value(const std::string& json, const std::string& key) {
    std::string k = "\"" + key + "\":";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return "";
    size_t s = json.find('"', pos + k.size());
    if (s == std::string::npos) return "";
    size_t e = json.find('"', s + 1);
    if (e == std::string::npos) return "";
    return json.substr(s + 1, e - s - 1);
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// Read manifest.json from a .xapp directory.
static bool read_manifest(const fs::path& bundle, std::string& name, std::string& id,
                           std::string& version, std::string& exec) {
    std::string content = read_file(bundle / "manifest.json");
    if (content.empty()) return false;
    name    = json_value(content, "name");
    id      = json_value(content, "identifier");
    version = json_value(content, "version");
    exec    = json_value(content, "executable");
    return !name.empty() && !id.empty();
}

// ─── Remote catalog helpers ───────────────────────────────────────────────────

static constexpr const char* kCatalogUrl = "https://pkg.freebsd-x.org/catalog.json";

static fs::path catalog_cache_path() {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".freebsdx-catalog.json" : fs::path("/tmp/.freebsdx-catalog.json");
}

struct RemoteEntry {
    std::string identifier, name, version, category, description, download_url;
    uint32_t size_kb{0};
};

static std::vector<RemoteEntry> load_catalog() {
    std::vector<RemoteEntry> entries;
    auto path = catalog_cache_path();
    if (!fs::exists(path)) return entries;
    auto json = read_file(path);
    if (json.empty()) return entries;

    std::string key = "\"packages\"";
    size_t arr_pos = json.find('[', json.find(key));
    if (arr_pos == std::string::npos) return entries;
    size_t arr_end = json.rfind(']');

    size_t i = arr_pos + 1;
    while (i < arr_end) {
        size_t obj_start = json.find('{', i);
        if (obj_start > arr_end || obj_start == std::string::npos) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
        RemoteEntry e;
        e.identifier   = json_value(obj, "identifier");
        e.name         = json_value(obj, "name");
        e.version      = json_value(obj, "version");
        e.category     = json_value(obj, "category");
        e.description  = json_value(obj, "description");
        e.download_url = json_value(obj, "download_url");
        std::string skb = json_value(obj, "size_kb");
        e.size_kb = skb.empty() ? 0 : static_cast<uint32_t>(std::stoul(skb));
        if (!e.identifier.empty()) entries.push_back(std::move(e));
        i = obj_end + 1;
    }
    return entries;
}

// ─── Package signing helpers ──────────────────────────────────────────────────

static fs::path trust_dir() {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".freebsdx-pkg-trust"
                : fs::path("/tmp/.freebsdx-pkg-trust");
}

static fs::path keys_dir() {
    return trust_dir() / "keys";
}

// Parse label from a minisign .pub or GPG .asc key file.
static std::string key_label(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return p.stem().string();
    std::string line;
    std::getline(f, line);
    const char* pfx = "untrusted comment: ";
    if (line.size() > std::strlen(pfx) &&
        line.compare(0, std::strlen(pfx), pfx) == 0)
        return line.substr(std::strlen(pfx));
    if (line.find("-----BEGIN PGP") != std::string::npos)
        return p.stem().string() + " (GPG)";
    return p.stem().string();
}

// Verify pkg against trusted keys in trust store.
// Returns {ok, message}.
static std::pair<bool,std::string> cli_verify(const fs::path& pkg) {
    std::string ms = pkg.string() + ".minisig";
    std::string gs = pkg.string() + ".sig";
    bool has_ms = fs::exists(ms);
    bool has_gs = !has_ms && fs::exists(gs);

    if (!has_ms && !has_gs)
        return {false, "no signature sidecar (.minisig or .sig)"};

    fs::path kdir = keys_dir();
    if (has_ms) {
        if (!fs::exists(kdir)) return {false, "trust store is empty"};
        bool found = false;
        for (const auto& e : fs::directory_iterator(kdir)) {
            if (!e.is_regular_file() || e.path().extension() != ".pub") continue;
            std::string cmd = "minisign -V -q -p '"
                            + e.path().string()
                            + "' -m '" + pkg.string()
                            + "' -x '" + ms
                            + "' 2>/dev/null";
            if (std::system(cmd.c_str()) == 0)
                return {true, "verified by key: " + key_label(e.path())};
            found = true; // at least one key was tried
        }
        (void)found;
        return {false, "signature not trusted by any key in the trust store"};
    }

    // GPG
    std::string cmd = "gpg --quiet --batch --verify '"
                    + gs + "' '" + pkg.string() + "' 2>/dev/null";
    return std::system(cmd.c_str()) == 0
           ? std::make_pair(true, std::string("verified via GPG"))
           : std::make_pair(false, std::string("GPG verification failed"));
}

int main(int argc, char* argv[]) {
    fbx::shared::Logger::init("xpkg");

    if (argc < 2) { print_usage(); return 0; }

    std::string command = argv[1];

    // Resolve apps root: prefer /usr/local/apps, fall back to ./apps or ../apps
    std::string apps_root = "/usr/local/apps";
    if (!fs::exists(apps_root)) {
        apps_root = fs::exists("./apps") ? "./apps" : "../apps";
    }

    AppDiscoveryService discovery;

    // ── list ─────────────────────────────────────────────────────────────────
    if (command == "list") {
        auto apps = discovery.discoverApps(apps_root);
        if (apps.empty()) {
            std::cout << "No apps installed in " << apps_root << "\n";
            return 0;
        }
        std::cout << "Installed apps in " << apps_root << ":\n";
        for (const auto& app : apps) {
            std::cout << "  " << app.name
                      << " [" << app.identifier << "]"
                      << " v" << app.version << "\n";
        }
    }

    // ── install ───────────────────────────────────────────────────────────────
    else if (command == "install") {
        if (argc < 3) { std::cerr << "Error: missing path\n"; return 1; }
        fs::path src(argv[2]);

        // .xpkg archive
        if (src.extension() == ".xpkg") {
            if (!fs::exists(src)) {
                std::cerr << "Error: file not found: " << src << "\n"; return 1;
            }

            // Extract to a temp directory
            char tmp_buf[] = "/tmp/xpkg-XXXXXX";
            if (!mkdtemp(tmp_buf)) {
                std::cerr << "Error: could not create temp dir\n"; return 1;
            }
            std::string tmp_dir = tmp_buf;

            std::string cmd = "tar -xzf '" + src.string() + "' -C '" + tmp_dir + "'";
            if (std::system(cmd.c_str()) != 0) {
                std::cerr << "Error: extraction failed\n";
                fs::remove_all(tmp_dir);
                return 1;
            }

            // Find .xapp inside extracted dir
            fs::path bundle;
            for (const auto& e : fs::directory_iterator(tmp_dir)) {
                if (e.is_directory() && e.path().extension() == ".xapp") {
                    bundle = e.path(); break;
                }
            }
            if (bundle.empty()) {
                std::cerr << "Error: no .xapp bundle found inside archive\n";
                fs::remove_all(tmp_dir); return 1;
            }

            std::string name, id, version, exec;
            if (!read_manifest(bundle, name, id, version, exec)) {
                std::cerr << "Error: invalid or missing manifest.json\n";
                fs::remove_all(tmp_dir); return 1;
            }

            fs::path dest = fs::path(apps_root) / bundle.filename();
            if (fs::exists(dest)) {
                std::cout << "Updating " << name << "…\n";
                fs::remove_all(dest);
            } else {
                std::cout << "Installing " << name << " v" << version << "…\n";
            }
            fs::copy(bundle, dest, fs::copy_options::recursive);
            fs::remove_all(tmp_dir);
            std::cout << "Done → " << dest << "\n";

        // .xapp directory
        } else if (fs::is_directory(src) && src.extension() == ".xapp") {
            std::string name, id, version, exec;
            if (!read_manifest(src, name, id, version, exec)) {
                std::cerr << "Error: invalid or missing manifest.json in " << src << "\n";
                return 1;
            }
            fs::path dest = fs::path(apps_root) / src.filename();
            if (fs::exists(dest)) {
                std::cout << "Updating " << name << "…\n";
                fs::remove_all(dest);
            } else {
                std::cout << "Installing " << name << " v" << version << "…\n";
            }
            fs::copy(src, dest, fs::copy_options::recursive);
            std::cout << "Done → " << dest << "\n";
        } else {
            std::cerr << "Error: source must be a .xpkg file or .xapp directory\n";
            return 1;
        }
    }

    // ── uninstall ─────────────────────────────────────────────────────────────
    else if (command == "uninstall") {
        if (argc < 3) { std::cerr << "Error: missing identifier\n"; return 1; }
        std::string target_id = argv[2];
        auto apps = discovery.discoverApps(apps_root);
        bool found = false;
        for (const auto& app : apps) {
            if (app.identifier == target_id) {
                std::cout << "Removing " << app.name << "…\n";
                fs::remove_all(app.bundle_path);
                std::cout << "Done.\n";
                found = true; break;
            }
        }
        if (!found) {
            std::cerr << "Error: no app with identifier '" << target_id << "'\n";
            return 1;
        }
    }

    // ── pack ──────────────────────────────────────────────────────────────────
    else if (command == "pack") {
        if (argc < 3) { std::cerr << "Error: missing .xapp directory\n"; return 1; }
        fs::path src(argv[2]);
        if (!fs::is_directory(src) || src.extension() != ".xapp") {
            std::cerr << "Error: source must be a .xapp directory\n"; return 1;
        }

        std::string name, id, version, exec;
        if (!read_manifest(src, name, id, version, exec)) {
            std::cerr << "Error: invalid or missing manifest.json\n"; return 1;
        }

        // Determine output archive name
        std::string out_name;
        if (argc >= 4) {
            out_name = argv[3];
        } else {
            // e.g. TextEditor.xapp → TextEditor-1.0.0.xpkg
            std::string stem = src.stem().string();
            out_name = stem + "-" + version + ".xpkg";
        }

        // tar -czf output.xpkg -C <parent> <dirname>
        fs::path parent = src.parent_path().empty() ? "." : src.parent_path();
        std::string cmd = "tar -czf '" + out_name + "' -C '" + parent.string()
                        + "' '" + src.filename().string() + "'";
        std::cout << "Packing " << name << " v" << version << " → " << out_name << "\n";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "Error: tar failed\n"; return 1;
        }
        std::cout << "Done.\n";
    }

    // ── info ──────────────────────────────────────────────────────────────────
    else if (command == "info") {
        if (argc < 3) { std::cerr << "Error: missing identifier or path\n"; return 1; }
        std::string arg = argv[2];

        // Could be a path to a .xapp dir or .xpkg file, or an installed identifier
        fs::path p(arg);
        std::string name, id, version, exec;
        bool found = false;

        if (p.extension() == ".xpkg" && fs::exists(p)) {
            // Extract just the manifest
            char tmp_buf[] = "/tmp/xpkg-info-XXXXXX";
            if (!mkdtemp(tmp_buf)) { std::cerr << "Error: temp dir failed\n"; return 1; }
            std::string tmp_dir = tmp_buf;
            std::string cmd = "tar -xzf '" + p.string() + "' -C '" + tmp_dir
                            + "' --include='*/manifest.json' 2>/dev/null || "
                              "tar -xzf '" + p.string() + "' -C '" + tmp_dir + "'";
            std::system(cmd.c_str());
            for (const auto& e : fs::directory_iterator(tmp_dir)) {
                if (e.is_directory() && e.path().extension() == ".xapp") {
                    found = read_manifest(e.path(), name, id, version, exec); break;
                }
            }
            fs::remove_all(tmp_dir);
        } else if (fs::is_directory(p) && p.extension() == ".xapp") {
            found = read_manifest(p, name, id, version, exec);
        } else {
            // Search by identifier in installed apps
            auto apps = discovery.discoverApps(apps_root);
            for (const auto& app : apps) {
                if (app.identifier == arg) {
                    name = app.name; id = app.identifier;
                    version = app.version; exec = app.executable_path;
                    found = true; break;
                }
            }
        }

        if (!found) {
            std::cerr << "Error: app not found: " << arg << "\n"; return 1;
        }
        std::cout << "Name:       " << name    << "\n"
                  << "Identifier: " << id      << "\n"
                  << "Version:    " << version << "\n"
                  << "Executable: " << exec    << "\n";
    }

    // ── search ────────────────────────────────────────────────────────────────
    else if (command == "search") {
        if (argc < 3) { std::cerr << "Error: missing search query\n"; return 1; }
        std::string query = argv[2];
        // case-insensitive: lower-case both sides
        std::string query_lower = query;
        std::transform(query_lower.begin(), query_lower.end(),
                       query_lower.begin(), ::tolower);

        auto apps = discovery.discoverApps(apps_root);
        bool any = false;
        for (const auto& app : apps) {
            std::string name_lower = app.name;
            std::string id_lower   = app.identifier;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(id_lower.begin(),   id_lower.end(),   id_lower.begin(),   ::tolower);
            if (name_lower.find(query_lower) != std::string::npos ||
                id_lower.find(query_lower)   != std::string::npos) {
                std::cout << "  " << app.name
                          << " [" << app.identifier << "]"
                          << " v" << app.version << "\n";
                any = true;
            }
        }
        if (!any) std::cout << "No matching apps found for '" << query << "'\n";
    }

    // ── update ────────────────────────────────────────────────────────────────
    else if (command == "update") {
        auto cache = catalog_cache_path();
        std::cout << "Fetching catalog from " << kCatalogUrl << "...\n";
        std::string cmd = "fetch -q -o \"" + cache.string() + "\" " + kCatalogUrl
                        + " 2>/dev/null || curl -s -o \"" + cache.string() + "\" " + kCatalogUrl;
        int ret = std::system(cmd.c_str());
        if (ret == 0 && fs::exists(cache)) {
            auto entries = load_catalog();
            std::cout << "Catalog updated: " << entries.size() << " packages available.\n";
            return 0;
        } else {
            std::cerr << "Failed to fetch catalog.\n";
            return 1;
        }
    }

    // ── remote-list ───────────────────────────────────────────────────────────
    else if (command == "remote-list") {
        auto entries = load_catalog();
        if (entries.empty()) {
            std::cout << "Catalog empty or not downloaded. Run: xpkg update\n";
            return 0;
        }
        std::cout << std::left
                  << std::setw(36) << "IDENTIFIER"
                  << std::setw(12) << "VERSION"
                  << std::setw(16) << "CATEGORY"
                  << "NAME\n";
        std::cout << std::string(80, '-') << "\n";
        for (const auto& e : entries) {
            std::cout << std::setw(36) << e.identifier
                      << std::setw(12) << e.version
                      << std::setw(16) << e.category
                      << e.name << "\n";
        }
        return 0;
    }

    // ── remote-search ─────────────────────────────────────────────────────────
    else if (command == "remote-search") {
        if (argc < 3) { std::cerr << "Usage: xpkg remote-search <query>\n"; return 1; }
        std::string q = argv[2];
        std::string ql = q;
        std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

        auto entries = load_catalog();
        bool found = false;
        for (const auto& e : entries) {
            std::string nl = e.name, dl = e.description, il = e.identifier;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
            std::transform(il.begin(), il.end(), il.begin(), ::tolower);
            if (nl.find(ql) != std::string::npos ||
                dl.find(ql) != std::string::npos ||
                il.find(ql) != std::string::npos) {
                std::cout << e.identifier << "  v" << e.version << "  [" << e.category << "]\n";
                std::cout << "  " << e.name << "\n";
                if (!e.description.empty())
                    std::cout << "  " << e.description << "\n";
                if (e.size_kb > 0)
                    std::cout << "  Size: " << e.size_kb << " KB\n";
                std::cout << "\n";
                found = true;
            }
        }
        if (!found) std::cout << "No packages matching '" << q << "'.\n";
        return 0;
    }

    // ── install-remote ────────────────────────────────────────────────────────
    else if (command == "install-remote") {
        if (argc < 3) { std::cerr << "Usage: xpkg install-remote <identifier>\n"; return 1; }
        std::string target_id = argv[2];

        auto entries = load_catalog();
        const RemoteEntry* found = nullptr;
        for (const auto& e : entries)
            if (e.identifier == target_id) { found = &e; break; }

        if (!found) {
            std::cerr << "Package '" << target_id << "' not found in catalog. Run: xpkg update\n";
            return 1;
        }

        std::cout << "Downloading " << found->name << " v" << found->version
                  << " (" << found->size_kb << " KB)...\n";

        std::string tmp = "/tmp/xpkg-" + found->identifier + ".xpkg";
        std::string dl_cmd = "fetch -q -o \"" + tmp + "\" \"" + found->download_url + "\""
                           + " 2>/dev/null || curl -s -o \"" + tmp + "\" \"" + found->download_url + "\"";
        if (std::system(dl_cmd.c_str()) != 0 || !fs::exists(tmp)) {
            std::cerr << "Download failed.\n"; return 1;
        }

        std::cout << "Installing...\n";
        // Exec ourselves with the install subcommand for the downloaded archive
        execl("/proc/self/exe", "xpkg", "install", tmp.c_str(), nullptr);
        // Fallback if /proc/self/exe not available
        std::string install_cmd = "xpkg install \"" + tmp + "\"";
        int ret = std::system(install_cmd.c_str());
        fs::remove(tmp);
        return ret == 0 ? 0 : 1;
    }

    // ── sign ──────────────────────────────────────────────────────────────────
    else if (command == "sign") {
        if (argc < 4) {
            std::cerr << "Usage: xpkg sign <pkg.xpkg> <secret.key>\n"; return 1;
        }
        fs::path pkg(argv[2]);
        std::string seckey(argv[3]);

        if (!fs::exists(pkg)) {
            std::cerr << "Error: package not found: " << pkg << "\n"; return 1;
        }
        if (!fs::exists(seckey)) {
            std::cerr << "Error: secret key not found: " << seckey << "\n"; return 1;
        }

        std::string cmd = "minisign -S -s '" + seckey
                        + "' -m '" + pkg.string() + "'";
        std::cout << "Signing " << pkg.filename().string() << "…\n";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "Error: minisign failed. Is minisign installed? (pkg install minisign)\n";
            return 1;
        }
        std::cout << "Signature written: " << pkg.string() << ".minisig\n";
    }

    // ── verify ────────────────────────────────────────────────────────────────
    else if (command == "verify") {
        if (argc < 3) { std::cerr << "Usage: xpkg verify <pkg.xpkg>\n"; return 1; }
        fs::path pkg(argv[2]);

        if (!fs::exists(pkg)) {
            std::cerr << "Error: package not found: " << pkg << "\n"; return 1;
        }

        auto [ok, msg] = cli_verify(pkg);
        if (ok) {
            std::cout << "✓ " << pkg.filename().string() << ": " << msg << "\n";
            return 0;
        } else {
            std::cerr << "✗ " << pkg.filename().string() << ": " << msg << "\n";
            return 1;
        }
    }

    // ── trust ─────────────────────────────────────────────────────────────────
    else if (command == "trust") {
        if (argc < 3) {
            std::cerr << "Usage: xpkg trust <pubkey.pub|key.asc>\n"; return 1;
        }
        fs::path src(argv[2]);
        if (!fs::exists(src)) {
            std::cerr << "Error: key file not found: " << src << "\n"; return 1;
        }

        fs::path kdir = keys_dir();
        try { fs::create_directories(kdir); }
        catch (...) { std::cerr << "Error: cannot create trust directory\n"; return 1; }

        std::string ext = src.extension().string();
        bool is_ms = (ext == ".pub");
        std::string dest_name = std::to_string(std::time(nullptr))
                              + (is_ms ? ".pub" : ".asc");
        fs::path dest = kdir / dest_name;

        try {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n"; return 1;
        }

        // For GPG keys also import into keyring
        if (!is_ms) {
            std::string cmd = "gpg --import '" + src.string() + "' 2>/dev/null";
            std::system(cmd.c_str());
        }

        std::cout << "Key trusted: " << key_label(dest) << "\n";
        std::cout << "Stored at:   " << dest << "\n";
    }

    // ── untrust ───────────────────────────────────────────────────────────────
    else if (command == "untrust") {
        if (argc < 3) {
            std::cerr << "Usage: xpkg untrust <fingerprint|filename-stem>\n"; return 1;
        }
        std::string target(argv[2]);
        fs::path kdir = keys_dir();

        if (!fs::exists(kdir)) {
            std::cerr << "Trust store is empty.\n"; return 1;
        }

        bool removed = false;
        for (const auto& e : fs::directory_iterator(kdir)) {
            if (!e.is_regular_file()) continue;
            if (e.path().stem().string() == target) {
                std::cout << "Removing key: " << key_label(e.path()) << "\n";
                fs::remove(e.path());
                removed = true;
                break;
            }
        }

        if (!removed) {
            std::cerr << "No key found with stem '" << target << "' in trust store.\n";
            std::cout << "Trusted keys in " << kdir << ":\n";
            for (const auto& e : fs::directory_iterator(kdir)) {
                if (e.is_regular_file())
                    std::cout << "  " << e.path().stem().string()
                              << "  — " << key_label(e.path()) << "\n";
            }
            return 1;
        }
        std::cout << "Done.\n";
    }

    // ── signing-policy ────────────────────────────────────────────────────────
    else if (command == "signing-policy") {
        fs::path policy_file = trust_dir() / "policy";

        if (argc < 3) {
            // Show current policy
            uint32_t req = 0, allow = 1;
            std::ifstream f(policy_file);
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("require_signed=", 0) == 0)
                    req = (line[15] == '1') ? 1u : 0u;
                else if (line.rfind("allow_unknown=", 0) == 0)
                    allow = (line[14] == '1') ? 1u : 0u;
            }
            std::cout << "require_signed: " << (req  ? "on" : "off") << "\n";
            std::cout << "allow_unknown:  " << (allow ? "on" : "off") << "\n";
            return 0;
        }

        std::string val(argv[2]);
        if (val != "on" && val != "off") {
            std::cerr << "Usage: xpkg signing-policy [on|off]\n"; return 1;
        }

        try { fs::create_directories(trust_dir()); } catch (...) {}
        std::ofstream f(policy_file);
        if (!f) { std::cerr << "Error: cannot write policy file\n"; return 1; }
        f << "require_signed=" << (val == "on" ? 1 : 0) << "\n";
        f << "allow_unknown="  << (val == "on" ? 0 : 1) << "\n";
        std::cout << "Package signing enforcement: " << val << "\n";
    }

    else {
        print_usage();
        return 1;
    }

    return 0;
}
