#pragma once
// The layout menu that opens under the green traffic light (macOS style tiling). ONE popup window for the whole app,
// reused by every covered window (never one per HWND). Layered, WS_EX_NOACTIVATE: it never takes focus from the app.
// Drawn with Direct2D into a DIB; no timer runs while it is closed or at rest. UI thread only.
// See docs/decisions/ADR-009-window-tiling.md.
#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include "animation/animation.h"
#include "app/window_tiler.h"
#include "ui/tile_menu_layout.h"

namespace mc {

class TilingMenu {
public:
    enum class State { Closed, Opening, Open, Closing };
    static constexpr UINT kMsgClosed = WM_APP + 45;  // posted to `notify` when the menu is closed

    ~TilingMenu() { destroy(); }
    void init(HINSTANCE inst, ID2D1Factory* factory);
    void destroy();

    struct Anchor {
        HWND target = nullptr;  // the window to tile
        HWND notify = nullptr;  // gets kMsgClosed
        Rect dot;               // the green dot, screen px
        Rect strip;             // the traffic-light strip, screen px: the menu opens below it
        double dpi = 1.0;       // scale, 1.0 = 96 DPI
        bool dark = true;       // theme: follows the title bar
        bool animate = true;    // false: Reduced Motion
    };
    void open(const Anchor& a);
    void close(HWND target, bool immediate = false);  // ignored when the menu belongs to another window
    void anchorHover(HWND target, bool over);         // the mouse is over the green dot / strip (keeps the menu open)
    State stateFor(HWND target) const { return target && target == target_ ? state_ : State::Closed; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(HWND, UINT, WPARAM, LPARAM);
    bool ensureWindow();
    void render();
    void animate();
    void schedule();
    void evalClose();
    void finishClose();
    void setHot(int i);
    void press(int i, bool down);
    void pick(int i);
    double now() const;
    void animTo(anim::AnimatedValue& v, double target, double seconds, anim::CubicBezier bez);

    HINSTANCE inst_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    HWND wnd_ = nullptr, target_ = nullptr, notify_ = nullptr;
    State state_ = State::Closed;
    TileMenuGeometry geo_;
    Rect screen_;  // menu rect on screen
    double dpi_ = 1.0;
    bool dark_ = true, animate_ = true, overMenu_ = false, overAnchor_ = false, animating_ = false, closeArmed_ = false, picked_ = false;
    int hot_ = -1, pressed_ = -1;
    anim::AnimatedValue openP_, hover_[kTileLayoutCount], press_[kTileLayoutCount];
    LARGE_INTEGER freq_{};
    WindowTiler tiler_;

    // DIB section + D2D DC render target.
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr, old_ = nullptr;
    int bw_ = 0, bh_ = 0;
    bool sizeDib(int w, int h);
    void releaseDib();
};

}  // namespace mc
