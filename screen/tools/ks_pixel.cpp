// SPDX-License-Identifier: MIT
//
// Reads one of SDL's BMP snapshots and answers a question about a rectangle of
// it, so a shell script can assert about pixels.
//
//   ks_pixel <file.bmp> modal <x> <y> <w> <h>   the commonest colour, as RRGGBB
//   ks_pixel <file.bmp> ink   <x> <y> <w> <h>   pixels that are not the modal one
//   ks_pixel <file.bmp> size                    WxH
//
// It is a test instrument, not part of the daemon: T35's oracles read pixels
// through SDL, and this is for the end-to-end run, where the only thing left
// of the window is a file.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // 0x00RRGGBB, top row first
};

uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool load(const char *path, Image &img)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    std::vector<uint8_t> d;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        d.insert(d.end(), chunk, chunk + n);
    fclose(f);

    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M')
        return false;
    uint32_t off = le32(&d[10]);
    int w        = int(le32(&d[18]));
    int h        = int(le32(&d[22]));
    uint32_t bpp = uint32_t(d[28]) | uint32_t(d[29]) << 8;
    if (bpp != 32 && bpp != 24)
        return false;

    bool bottom_up = h > 0;
    int rows       = bottom_up ? h : -h;
    size_t stride  = ((size_t(w) * bpp / 8 + 3) / 4) * 4;
    img.w          = w;
    img.h          = rows;
    img.px.assign(size_t(w) * rows, 0);

    for (int y = 0; y < rows; y++) {
        size_t src = off + size_t(bottom_up ? rows - 1 - y : y) * stride;
        if (src + stride > d.size())
            return false;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &d[src + size_t(x) * (bpp / 8)];
            img.px[size_t(y) * w + x] = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ks_pixel <file.bmp> modal|ink|size [x y w h]\n");
        return 2;
    }
    Image img;
    if (!load(argv[1], img)) {
        fprintf(stderr, "ks_pixel: %s: not a 24- or 32-bit BMP\n", argv[1]);
        return 2;
    }

    std::string what = argv[2];
    if (what == "size") {
        printf("%dx%d\n", img.w, img.h);
        return 0;
    }
    if (argc < 7) {
        fprintf(stderr, "ks_pixel: %s needs x y w h\n", what.c_str());
        return 2;
    }
    int x0 = atoi(argv[3]), y0 = atoi(argv[4]);
    int w = atoi(argv[5]), h = atoi(argv[6]);
    if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0 || x0 + w > img.w || y0 + h > img.h) {
        fprintf(stderr, "ks_pixel: the rectangle is outside %dx%d\n", img.w, img.h);
        return 2;
    }

    std::map<uint32_t, int> count;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            count[img.px[size_t(y) * img.w + x]]++;

    uint32_t best = 0;
    int most      = 0;
    for (const auto &kv : count)
        if (kv.second > most) {
            most = kv.second;
            best = kv.first;
        }

    if (what == "modal") {
        printf("%06x\n", best);
        return 0;
    }
    if (what == "ink") {
        printf("%d\n", w * h - most);
        return 0;
    }
    fprintf(stderr, "ks_pixel: no such question: %s\n", what.c_str());
    return 2;
}
