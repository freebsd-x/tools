// icon_montage — render one or more icon files (SVG or PNG) through xrender's
// ImageDecoder and tile them into a single PNG for visual inspection.
//
//     icon_montage OUT.png [--size N] ICON [ICON ...]
//
// SVGs are rasterized by SvgEngine at N×N (default 64); PNGs are decoded as-is.
// Tiles are laid left-to-right over a transparent background with a small
// gutter. Handy for eyeballing app-icon bundles the way the shell will show
// them — the companion to tools/icon_gen.cc, which authors the SVGs.
#include "xrender/image_decoder.h"

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string out;
    std::vector<std::string> icons;
    uint32_t size = 64;
    constexpr uint32_t gutter = 8;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            size = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (out.empty()) {
            out = std::move(a);
        } else {
            icons.push_back(std::move(a));
        }
    }
    if (out.empty() || icons.empty() || size == 0) {
        std::fprintf(stderr, "usage: icon_montage OUT.png [--size N] ICON [ICON ...]\n");
        return 2;
    }

    struct Tile { uint32_t w, h; std::vector<uint8_t> rgba; };
    std::vector<Tile> tiles;
    for (const auto& path : icons) {
        auto res = fbx::xrender::ImageDecoder::decode(path, size, size);
        if (!res) {
            std::fprintf(stderr, "icon_montage: failed to decode %s\n", path.c_str());
            return 1;
        }
        tiles.push_back({res->width_, res->height_, std::move(res->pixels_)});
    }

    uint32_t canvas_w = 0, canvas_h = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        canvas_h = std::max(canvas_h, tiles[i].h);
        canvas_w += tiles[i].w + (i ? gutter : 0);
    }

    // Transparent RGBA canvas; short tiles leave the rows below them clear.
    std::vector<uint8_t> canvas(static_cast<size_t>(canvas_w) * canvas_h * 4, 0);
    uint32_t xoff = 0;
    for (const auto& t : tiles) {
        for (uint32_t y = 0; y < t.h; ++y) {
            std::memcpy(&canvas[(static_cast<size_t>(y) * canvas_w + xoff) * 4],
                        &t.rgba[static_cast<size_t>(y) * t.w * 4],
                        static_cast<size_t>(t.w) * 4);
        }
        xoff += t.w + gutter;
    }

    FILE* fp = std::fopen(out.c_str(), "wb");
    if (!fp) {
        std::fprintf(stderr, "icon_montage: cannot open %s for writing\n", out.c_str());
        return 1;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : nullptr);
        std::fclose(fp);
        return 1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, canvas_w, canvas_h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows(canvas_h);
    for (uint32_t y = 0; y < canvas_h; ++y) {
        rows[y] = &canvas[static_cast<size_t>(y) * canvas_w * 4];
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);

    std::printf("icon_montage: wrote %s (%ux%u, %zu tiles)\n",
                out.c_str(), canvas_w, canvas_h, tiles.size());
    return 0;
}
