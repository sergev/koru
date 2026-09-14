// SPDX-License-Identifier: MIT

#include "window.h"

#include <cstdlib>
#include <cstring>

namespace {

u32 scale_from_env()
{
    const char *s = getenv("KORU_SCREEN_SCALE");
    if (!s)
        return 2;
    long v = strtol(s, nullptr, 10);
    return (v >= 1 && v <= 8) ? u32(v) : 2;
}

bool make_texture(Win &w)
{
    if (w.texture)
        SDL_DestroyTexture(w.texture);
    w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, int(w.r.w), int(w.r.h));
    if (!w.texture)
        return false;
    // Nearest, because a bitmap font scaled with anything else is a smear and
    // the ink oracle would stop meaning what it says.
    SDL_SetTextureScaleMode(w.texture, SDL_SCALEMODE_NEAREST);
    return true;
}

// SDL's named keys, in this protocol's numbering. Everything else is a
// codepoint, which is what makes the two spaces impossible to confuse.
uint32_t named_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return KS_KEY_ENTER;
    case SDLK_BACKSPACE:
        return KS_KEY_BACKSPACE;
    case SDLK_TAB:
        return KS_KEY_TAB;
    case SDLK_ESCAPE:
        return KS_KEY_ESCAPE;
    case SDLK_DELETE:
        return KS_KEY_DELETE;
    case SDLK_INSERT:
        return KS_KEY_INSERT;
    case SDLK_UP:
        return KS_KEY_UP;
    case SDLK_DOWN:
        return KS_KEY_DOWN;
    case SDLK_LEFT:
        return KS_KEY_LEFT;
    case SDLK_RIGHT:
        return KS_KEY_RIGHT;
    case SDLK_HOME:
        return KS_KEY_HOME;
    case SDLK_END:
        return KS_KEY_END;
    case SDLK_PAGEUP:
        return KS_KEY_PAGE_UP;
    case SDLK_PAGEDOWN:
        return KS_KEY_PAGE_DOWN;
    case SDLK_F1:
        return KS_KEY_F1;
    case SDLK_F2:
        return KS_KEY_F2;
    case SDLK_F3:
        return KS_KEY_F3;
    case SDLK_F4:
        return KS_KEY_F4;
    case SDLK_F5:
        return KS_KEY_F5;
    case SDLK_F6:
        return KS_KEY_F6;
    case SDLK_F7:
        return KS_KEY_F7;
    case SDLK_F8:
        return KS_KEY_F8;
    case SDLK_F9:
        return KS_KEY_F9;
    case SDLK_F10:
        return KS_KEY_F10;
    case SDLK_F11:
        return KS_KEY_F11;
    case SDLK_F12:
        return KS_KEY_F12;
    default:
        return 0;
    }
}

} // namespace

bool win_open(Win &w, const char *title, u32 cols, u32 rows, u32 scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return false;
    if (!render_resize(w.r, cols, rows, scale ? scale : scale_from_env()))
        return false;
    if (!SDL_CreateWindowAndRenderer(title, int(w.r.w), int(w.r.h), SDL_WINDOW_RESIZABLE,
                                     &w.window, &w.renderer))
        return false;
    return make_texture(w);
}

void win_close(Win &w)
{
    if (w.texture)
        SDL_DestroyTexture(w.texture);
    if (w.renderer)
        SDL_DestroyRenderer(w.renderer);
    if (w.window)
        SDL_DestroyWindow(w.window);
    w.texture  = nullptr;
    w.renderer = nullptr;
    w.window   = nullptr;
    SDL_Quit();
}

bool win_resize(Win &w, u32 cols, u32 rows)
{
    if (cols == w.r.cols && rows == w.r.rows)
        return true;
    if (!render_resize(w.r, cols, rows, w.r.scale))
        return false;
    // The window too, not only the texture: the renderer's output is the
    // window's size, so a texture that outgrew it would be scaled down and
    // every pixel oracle would be reading an interpolation.
    SDL_SetWindowSize(w.window, int(w.r.w), int(w.r.h));
    SDL_SyncWindow(w.window);
    return make_texture(w);
}

void win_present(Win &w, Term &t)
{
    render_damage(w.r, t, screen_flush(t));

    SDL_UpdateTexture(w.texture, nullptr, w.r.px.data(), int(w.r.w * sizeof(uint32_t)));
    SDL_RenderClear(w.renderer);
    SDL_RenderTexture(w.renderer, w.texture, nullptr, nullptr);
    SDL_RenderPresent(w.renderer);
}

bool win_key(const SDL_KeyboardEvent &e, KsKey &out)
{
    uint32_t mods = 0;
    if (e.mod & SDL_KMOD_SHIFT)
        mods |= KS_MOD_SHIFT;
    if (e.mod & SDL_KMOD_CTRL)
        mods |= KS_MOD_CTRL;
    if (e.mod & SDL_KMOD_ALT)
        mods |= KS_MOD_ALT;
    if (e.mod & SDL_KMOD_GUI)
        mods |= KS_MOD_META;

    uint32_t code = named_key(e.key);
    if (!code) {
        // A printable key carries its codepoint — and it is the *unmodified*
        // one: ^C is 'c' with KS_MOD_CTRL, never byte 3, which is the premise
        // the whole cell model rests on. Shift is applied, because a capital
        // is a different character and not a modified one.
        SDL_Keycode k = e.key;
        if (mods & KS_MOD_SHIFT) {
            SDL_Keycode shifted = SDL_GetKeyFromScancode(e.scancode, e.mod, true);
            if (shifted >= ' ' && shifted < KS_KEY_NAMED)
                k = shifted;
        }
        if (k < ' ' || k >= KS_KEY_NAMED)
            return false; // a bare modifier, or a key with nothing to carry
        code = uint32_t(k);
        // A shifted letter is the capital itself, so the modifier has been
        // spent; a shifted command key keeps it.
        if ((mods & KS_MOD_SHIFT) && uint32_t(k) != uint32_t(e.key))
            mods &= ~uint32_t(KS_MOD_SHIFT);
    }

    out.code = code;
    out.mods = mods;
    return true;
}

void win_grid_of(const Win &w, int pixels_w, int pixels_h, u32 &cols, u32 &rows)
{
    cols = pixels_w > 0 && w.r.cw ? u32(pixels_w) / w.r.cw : 0;
    rows = pixels_h > 0 && w.r.ch ? u32(pixels_h) / w.r.ch : 0;
    if (!cols)
        cols = 1;
    if (!rows)
        rows = 1;
}

bool win_read(Win &w, std::vector<uint32_t> &out)
{
    SDL_Surface *s = SDL_RenderReadPixels(w.renderer, nullptr);
    if (!s)
        return false;
    // What came back must be the grid's own size. A mismatch means the window
    // and the texture have drifted apart, and every oracle downstream would be
    // reading the wrong pixels rather than failing.
    if (u32(s->w) != w.r.w || u32(s->h) != w.r.h) {
        SDL_DestroySurface(s);
        return false;
    }
    // The renderer picks its own format — the software one answers ARGB and
    // the GL one ABGR — so convert rather than trusting either.
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(s);
    if (!c)
        return false;

    out.assign(size_t(c->w) * c->h, 0);
    for (int y = 0; y < c->h; y++)
        memcpy(&out[size_t(y) * c->w], static_cast<const uint8_t *>(c->pixels) + size_t(y) * c->pitch,
               size_t(c->w) * sizeof(uint32_t));
    SDL_DestroySurface(c);
    return true;
}

bool win_snapshot(Win &w, const char *path)
{
    SDL_Surface *s = SDL_RenderReadPixels(w.renderer, nullptr);
    if (!s)
        return false;
    bool ok = SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    return ok;
}
