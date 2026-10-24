// icon_gen — single source of truth for .xapp bundle metadata.
//
// Generates a CANONICAL manifest.json + Resources/icon.svg for every app bundle
// from one declarative table, plus the shell branding assets. Re-run after adding
// an app or changing its metadata so every bundle stays structurally consistent:
//   manifest schema {name, version, identifier, executable, icon, permissions,
//   mime_types} and a 128×128 rounded-rect SVG icon (distinct colour + glyph).
//
// Build: ninja -C out icon_gen ; Run: LD_LIBRARY_PATH=./out ./out/icon_gen

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cctype>

namespace {

const std::string kUI      = "com.freebsdx.permission.UI";
const std::string kStorage = "com.freebsdx.permission.STORAGE_WRITE";
const std::string kPower   = "com.freebsdx.permission.POWER_READ";

// One app bundle. identifier = com.freebsdx.<id>, executable = bin/<id>.
struct App {
    std::string dir;          // bundle stem, e.g. "Settings" → apps/Settings.xapp
    std::string id;           // lowercase slug, e.g. "settings"
    std::string name;         // display name
    std::string color;        // icon background (hex)
    std::string glyph;        // SVG path for the white glyph; "" → initial letter
    std::vector<std::string> permissions;
    std::vector<std::string> mime_types;
};


void writeIcon(const App& a) {
    const std::string path = "apps/" + a.dir + ".xapp/Resources/icon.svg";
    std::ofstream f(path);
    if (!f) { std::cerr << "[icon_gen] cannot write " << path << "\n"; return; }
    f << "<svg width=\"128\" height=\"128\" viewBox=\"0 0 100 100\" xmlns=\"http://www.w3.org/2000/svg\">\n"
      << "  <defs>\n"
      << "    <linearGradient id=\"g\" x1=\"0%\" y1=\"0%\" x2=\"100%\" y2=\"100%\">\n"
      << "      <stop offset=\"0%\" style=\"stop-color:" << a.color << ";stop-opacity:1\" />\n"
      << "      <stop offset=\"100%\" style=\"stop-color:#000000;stop-opacity:0.45\" />\n"
      << "    </linearGradient>\n"
      << "  </defs>\n"
      << "  <rect width=\"100\" height=\"100\" rx=\"22\" fill=\"url(#g)\" />\n";
    // The SVG engine renders filled <path> only (no <text>), so every glyph is a
    // path; an empty glyph falls back to a plain filled circle placeholder.
    const std::string g = a.glyph.empty()
        ? "M50 32 A18 18 0 1 0 50 68 A18 18 0 1 0 50 32 Z"
        : a.glyph;
    f << "  <path d=\"" << g << "\" fill=\"white\" fill-opacity=\"0.92\" />\n";
    f << "</svg>\n";
    std::cout << "[icon_gen] icon     " << path << "\n";
}

std::string jsonArray(const std::vector<std::string>& v) {
    if (v.empty()) return "[]";
    std::string s = "[\n";
    for (size_t i = 0; i < v.size(); ++i)
        s += "    \"" + v[i] + "\"" + (i + 1 < v.size() ? ",\n" : "\n");
    return s + "  ]";
}

void writeManifest(const App& a) {
    const std::string path = "apps/" + a.dir + ".xapp/manifest.json";
    std::ofstream f(path);
    if (!f) { std::cerr << "[icon_gen] cannot write " << path << "\n"; return; }
    f << "{\n"
      << "  \"name\": \"" << a.name << "\",\n"
      << "  \"version\": \"1.0.0\",\n"
      << "  \"identifier\": \"com.freebsdx." << a.id << "\",\n"
      << "  \"executable\": \"bin/" << a.id << "\",\n"
      << "  \"icon\": \"Resources/icon.svg\",\n"
      << "  \"permissions\": " << jsonArray(a.permissions) << ",\n"
      << "  \"mime_types\": " << jsonArray(a.mime_types) << "\n"
      << "}\n";
    std::cout << "[icon_gen] manifest " << path << "\n";
}

void writeImage(const std::string& path, uint32_t w, uint32_t h, const std::string& content) {
    std::ofstream f(path);
    if (!f) return;
    f << "<svg width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h
      << "\" xmlns=\"http://www.w3.org/2000/svg\">\n" << content << "</svg>\n";
    std::cout << "[icon_gen] asset    " << path << "\n";
}

}  // namespace

int main() {
    // ── App bundle table (keep alphabetical). glyph "" → initial-letter icon. ────
    const std::vector<App> apps = {
        {"AppManager",  "appmanager",  "App Manager",      "#5a6b8c", "M28 28 H46 V46 H28 Z M54 28 H72 V46 H54 Z M28 54 H46 V72 H28 Z M54 54 H72 V72 H54 Z", {kUI}, {}},
        {"Archive",     "archive",     "Archive",          "#b5651d", "M28 30 H72 V40 H28 Z M32 44 H68 V74 H32 Z M46 44 H54 V58 H46 Z", {kUI, kStorage}, {"application/zip","application/x-tar","application/gzip"}},
        {"Browser",     "browser",     "Web Browser",      "#1a73e8", "M50 26 A24 24 0 1 0 50 74 A24 24 0 1 0 50 26 Z", {kUI}, {"text/html"}},
        {"Calculator",  "calculator",  "Calculator",       "#00aa55", "M45 30 L55 30 L55 45 L70 45 L70 55 L55 55 L55 70 L45 70 L45 55 L30 55 L30 45 L45 45 Z", {kUI}, {}},
        {"Calendar",    "calendar",    "Calendar",         "#e8453c", "M28 32 H72 V40 H28 Z M28 44 H72 V72 H28 Z M36 50 H44 V58 H36 Z M56 50 H64 V58 H56 Z", {kUI}, {}},
        {"Camera",      "camera",      "Camera",           "#444444", "M30 40 H42 L46 34 H54 L58 40 H70 V68 H30 Z M50 46 A10 10 0 1 1 50 66 A10 10 0 1 1 50 46 Z", {kUI}, {}},
        {"ColorPicker", "colorpicker", "Color Picker",     "#9c27b0", "M58 28 L72 42 L46 68 L32 68 L32 54 Z M30 72 H56 V76 H30 Z", {kUI}, {}},
        {"Contacts",    "contacts",    "Contacts",         "#0097a7", "M50 30 A11 11 0 1 1 50 52 A11 11 0 1 1 50 30 Z M28 74 A22 18 0 0 1 72 74 Z", {kUI}, {}},
        {"DemoApp",     "demoapp",     "DemoApp",          "#0078d7", "M50 20 L60 40 L80 40 L65 55 L70 75 L50 65 L30 75 L35 55 L20 40 L40 40 Z", {kUI}, {}},
        {"DiskManager", "diskmanager", "Disk Manager",     "#607d8b", "M30 34 H70 V66 H30 Z M38 42 H62 V50 H38 Z M38 56 H48 V60 H38 Z", {kUI, kStorage}, {}},
        {"Files",       "files",       "Files",            "#42a5f5", "M28 34 H46 L52 40 H72 V70 H28 Z", {kUI, kStorage}, {}},
        {"FontManager", "fontmanager", "Font Manager",     "#5d4037", "M38 72 L50 30 L62 72 H54 L50 58 L46 72 Z M44 52 H56 V58 H44 Z", {kUI}, {}},
        {"GpuDemo",     "gpudemo",     "GPU Demo",         "#388e3c", "M32 36 H68 V64 H32 Z M40 44 H60 V56 H40 Z M26 46 H32 V54 H26 Z M68 46 H74 V54 H68 Z", {kUI}, {}},
        {"ImageViewer", "imageviewer", "Image Viewer",     "#ff7043", "M28 32 H72 V68 H28 Z M38 44 A5 5 0 1 1 38 54 A5 5 0 1 1 38 44 Z M34 64 L48 50 L58 60 L66 52 L66 64 Z", {kUI}, {"image/png","image/jpeg","image/gif","image/webp","image/svg+xml","image/bmp"}},
        {"Installer",   "installer",   "System Installer", "#455a64", "M50 28 L58 48 H42 Z M44 48 H56 V62 H44 Z M34 68 H66 V74 H34 Z", {kUI, kStorage}, {}},
        {"Mail",        "mail",        "Mail",             "#1e88e5", "M28 34 H72 V66 H28 Z M28 34 L50 52 L72 34", {kUI}, {}},
        {"Maps",        "maps",        "Maps",             "#43a047", "M50 26 A16 16 0 0 1 66 42 C66 58 50 76 50 76 C50 76 34 58 34 42 A16 16 0 0 1 50 26 Z M50 36 A6 6 0 1 1 50 48 A6 6 0 1 1 50 36 Z", {kUI}, {}},
        {"MusicPlayer", "musicplayer", "Music",            "#ab47bc", "M44 30 L70 26 V60 A8 8 0 1 1 62 52 V38 L52 40 V66 A8 8 0 1 1 44 58 Z", {kUI}, {"audio/mpeg","audio/flac","audio/wav","audio/ogg"}},
        {"Notes",       "notes",       "Notes",            "#f9a825", "M32 28 H68 V72 H32 Z M40 40 H60 V44 H40 Z M40 50 H60 V54 H40 Z M40 60 H52 V64 H40 Z", {kUI}, {}},
        {"PDFViewer",   "pdfviewer",   "PDF Viewer",       "#c62828", "M34 28 H58 L68 38 V72 H34 Z M42 50 H58 V54 H42 Z M42 58 H58 V62 H42 Z", {kUI}, {"application/pdf"}},
        {"Settings",    "settings",    "System Settings",  "#4a4a4a", "M50 35 A15 15 0 1 1 50 65 A15 15 0 1 1 50 35 M45 20 L55 20 L57 30 L43 30 Z M45 70 L55 70 L57 80 L43 80 Z", {kUI, kPower}, {}},
        {"TaskManager", "taskmanager", "Task Manager",     "#00838f", "M30 70 H38 V50 H30 Z M46 70 H54 V34 H46 Z M62 70 H70 V44 H62 Z", {kUI}, {}},
        {"Terminal",    "terminal",    "Terminal",         "#333333", "M28 30 H72 V70 H28 Z M34 42 L44 50 L34 58 M48 58 H64 V62 H48 Z", {kUI}, {"application/x-sh","text/x-shellscript"}},
        {"TextEditor",  "texteditor",  "Text Editor",      "#3949ab", "M34 28 H58 L68 38 V72 H34 Z M42 46 H60 V50 H42 Z M42 54 H60 V58 H42 Z M42 62 H54 V66 H42 Z", {kUI},
            {"text/plain","text/markdown","text/html","text/css","text/javascript","text/x-c++","text/x-c","text/x-python","text/x-rust","text/x-go","application/json","application/xml","application/x-sh"}},
        {"VideoPlayer", "videoplayer", "Video",            "#d81b60", "M30 34 H70 V66 H30 Z M44 44 L60 50 L44 56 Z", {kUI}, {"video/mp4","video/x-matroska","video/webm"}},
        {"Weather",     "weather",     "Weather",          "#039be5", "M40 54 A10 10 0 0 1 44 36 A12 12 0 0 1 66 44 A9 9 0 0 1 64 62 H42 A9 9 0 0 1 40 54 Z", {kUI}, {}},
    };

    for (const auto& a : apps) {
        std::filesystem::create_directories("apps/" + a.dir + ".xapp/Resources");
        writeManifest(a);
        writeIcon(a);
    }

    // ── Shell branding assets ────────────────────────────────────────────────────
    std::filesystem::create_directories("core/shell/Resources/Branding");
    std::string x_logo =
        "  <defs>\n"
        "    <linearGradient id=\"xgrad\" x1=\"0%\" y1=\"0%\" x2=\"100%\" y2=\"100%\">\n"
        "      <stop offset=\"0%\" style=\"stop-color:#ffffff;stop-opacity:1\" />\n"
        "      <stop offset=\"100%\" style=\"stop-color:#0078d7;stop-opacity:1\" />\n"
        "    </linearGradient>\n"
        "  </defs>\n"
        "  <path d=\"M20 20 L80 80 M80 20 L20 80\" stroke=\"url(#xgrad)\" stroke-width=\"12\" stroke-linecap=\"round\" />\n";
    writeImage("core/shell/Resources/Branding/logo_x.svg", 100, 100, x_logo);
    writeImage("core/shell/Resources/Branding/wallpaper.svg", 1920, 1080,
        "  <defs>\n"
        "    <linearGradient id=\"wp\" x1=\"0%\" y1=\"0%\" x2=\"100%\" y2=\"100%\">\n"
        "      <stop offset=\"0%\" style=\"stop-color:#0a2a5e;stop-opacity:1\" />\n"
        "      <stop offset=\"55%\" style=\"stop-color:#123a7a;stop-opacity:1\" />\n"
        "      <stop offset=\"100%\" style=\"stop-color:#081634;stop-opacity:1\" />\n"
        "    </linearGradient>\n"
        "  </defs>\n"
        "  <rect width=\"1920\" height=\"1080\" fill=\"url(#wp)\" />\n"
        "  <circle cx=\"1500\" cy=\"240\" r=\"520\" fill=\"#2f6fc0\" fill-opacity=\"0.20\" />\n"
        "  <circle cx=\"300\" cy=\"900\" r=\"460\" fill=\"#4a3f9c\" fill-opacity=\"0.18\" />\n");
    writeImage("core/shell/Resources/Branding/lockscreen.svg", 1920, 1080,
        "  <rect width=\"1920\" height=\"1080\" fill=\"#010204\" />\n"
        "  <g transform=\"translate(860, 440) scale(2)\">\n" + x_logo + "  </g>\n");

    std::cout << "[icon_gen] done: " << apps.size() << " bundles.\n";
    return 0;
}
