// Cursor glyph preview — rasterizes the diagonal resize cursors to ASCII so the
// shapes can be eyeballed without a display. '#' = white fill, '.' = black
// outline, ' ' = transparent. The '+' marks the hotspot pixel.
//
// This links only cursor_glyphs.cc (no DRM), so it runs anywhere.

#include "xserver/cursor_glyphs.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace fbx::xserver;

// Render helper: takes any glyph function that fills a buffer + reports hotspot.
template <typename Fn>
static void dumpGlyph(const char* title, Fn&& draw, uint32_t N = 24) {
    // Block glyphs live in 0..19 at sc=1 (N=24 is plenty); the AA arrow reaches
    // ~28x32 with its shadow, so it passes a larger window.
    std::vector<uint32_t> px(N * N, 0);
    int32_t hx = 0, hy = 0;
    draw(px.data(), N, N, /*scale_blocks=*/1, hx, hy);

    std::cout << "\n" << title << "   (hotspot at " << hx << "," << hy << ")\n";
    for (uint32_t y = 0; y < N - 2; ++y) {
        std::cout << "  ";
        for (uint32_t x = 0; x < N - 2; ++x) {
            uint32_t p = px[y * N + x];
            char c;
            if (p == 0)                 c = ((int)x == hx && (int)y == hy) ? '+' : ' ';
            else if ((p & 0x00FFFFFF))   c = '#';   // has colour -> white fill
            else                         c = '.';   // opaque black -> outline
            std::cout << c;
        }
        std::cout << "\n";
    }
}

int main() {
    std::cout << "=== Cursor glyph preview ===\n";
    dumpGlyph("NW<->SE  (nw/se/nwse_resize, shapes 21/23/29)",
              [](uint32_t* px, uint32_t s, uint32_t n, int sc, int32_t& hx, int32_t& hy) {
                  DrawDiagonalResize(px, s, n, sc, /*ne_sw=*/false, hx, hy);
              });
    dumpGlyph("NE<->SW  (ne/sw/nesw_resize, shapes 20/24/28)",
              [](uint32_t* px, uint32_t s, uint32_t n, int sc, int32_t& hx, int32_t& hy) {
                  DrawDiagonalResize(px, s, n, sc, /*ne_sw=*/true, hx, hy);
              });
    dumpGlyph("not_allowed  (shape 15)", DrawNotAllowed);
    dumpGlyph("grab  (shape 16, open hand)", DrawGrab);
    dumpGlyph("grabbing  (shape 17)", DrawGrabbing);
    dumpGlyph("zoom_in  (shape 33)",
              [](uint32_t* px, uint32_t s, uint32_t n, int sc, int32_t& hx, int32_t& hy) {
                  DrawZoom(px, s, n, sc, /*zoom_in=*/true, hx, hy);
              });
    dumpGlyph("zoom_out  (shape 34)",
              [](uint32_t* px, uint32_t s, uint32_t n, int sc, int32_t& hx, int32_t& hy) {
                  DrawZoom(px, s, n, sc, /*zoom_in=*/false, hx, hy);
              });
    dumpGlyph("wait  (shape 6, hourglass)", DrawWait);
    dumpGlyph("progress  (shape 5, arrow + busy)", DrawProgress);
    dumpGlyph("copy  (shape 12, arrow + '+')", DrawCopy);
    dumpGlyph("alias  (shape 11, arrow + link)", DrawAlias);
    dumpGlyph("no_drop  (shape 14, arrow + no)", DrawNoDrop);
    dumpGlyph("move  (shape 13, fleur)", DrawMove);
    dumpGlyph("cell  (shape 7, thick plus)", DrawCell);
    dumpGlyph("all_scroll  (shape 32)", DrawAllScroll);
    dumpGlyph("help  (shape 3, arrow + '?')", DrawHelp);
    dumpGlyph("context_menu  (shape 2, arrow + menu)", DrawContextMenu);
    dumpGlyph("text  (shape 9, vertical I-beam)", DrawText);
    dumpGlyph("vertical_text  (shape 10, horizontal I-beam)", DrawVerticalText);
    dumpGlyph("crosshair  (shape 8)", DrawCrosshair);
    dumpGlyph("h_resize  (e/w/ew/col_resize)", DrawHResize);
    dumpGlyph("v_resize  (n/s/ns/row_resize)", DrawVResize);
    dumpGlyph("default arrow  (AA, shadow)",
              [](uint32_t* p, uint32_t s, uint32_t n, int, int32_t& hx, int32_t& hy) {
                  DrawArrow(p, s, n, /*scale=*/1.0f, hx, hy);
              },
              /*N=*/34);
    return 0;
}
