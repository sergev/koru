// SPDX-License-Identifier: MIT
//
// The SDL3 half: a window, a texture, keys normalised to {code, mods}, and a
// resize that gives back a grid rather than pixels.
//
// Everything that draws is in render.h and names no SDL. What is here is the
// platform, so the pixel oracles can go through it — SDL_RenderReadPixels on
// the offscreen driver — without the drawing depending on it.
#pragma once

#include "render.h"
#include "screen.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

struct Win {
    SDL_Window *window     = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture   = nullptr;
    Render r;
};

// A key as a Braam program sees it: no control characters, so ^C is 'c' with
// KS_MOD_CTRL and the reader decides what that means.
struct KsKey {
    uint32_t code = 0;
    uint32_t mods = 0;
};

// Opens the window for a grid of this size. `scale` is the pixel multiplier;
// 0 asks the environment ($KORU_SCREEN_SCALE), and then 2.
bool win_open(Win &w, const char *title, u32 cols, u32 rows, u32 scale);

void win_close(Win &w);

// Sizes the texture and the pixel buffer to a new grid.
bool win_resize(Win &w, u32 cols, u32 rows);

// Flushes the terminal's damage, draws it, uploads it and presents. The whole
// texture is uploaded, but only the damage is drawn: what a cell outside it
// holds is whatever the last frame left, which is the point.
void win_present(Win &w, Term &t);

// A key event, normalised. False for a key that carries nothing — a bare
// modifier, or one this table does not name.
bool win_key(const SDL_KeyboardEvent &e, KsKey &out);

// The grid a window of this pixel size holds, never zero in either direction.
void win_grid_of(const Win &w, int pixels_w, int pixels_h, u32 &cols, u32 &rows);

// The frame as ARGB8888, read back through SDL. For the pixel oracles, and for
// the snapshot $KORU_SCREEN_SNAP asks for.
bool win_read(Win &w, std::vector<uint32_t> &out);

// Writes the frame to `path` as a BMP. Called when $KORU_SCREEN_SNAP is set.
bool win_snapshot(Win &w, const char *path);
