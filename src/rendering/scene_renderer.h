#pragma once
// GPU chrome of the overview, drawn with Direct2D into DirectComposition surfaces. Static content
// (wallpaper backdrop, labels, desktop strip, focus ring) is rasterized once when it changes; every
// frame only sets offsets, transforms and opacity. DWM draws window thumbnails above all of this.
// Owned and used by the compositor thread only.
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "core/geometry.h"

namespace mc {

using Microsoft::WRL::ComPtr;

struct BackdropSpec {
    Rect monitor;            // full monitor rect, overlay coordinates
    std::wstring wallpaper;  // image file; empty = solid color
    COLORREF color = 0;      // used when there is no image or it fails to load
    bool operator==(const BackdropSpec&) const = default;
};

struct BackdropStyle {
    double dim = 0.55;          // black over the wallpaper, 0..1
    bool blur = false;
    double blurIntensity = 0;   // 0..1
    double parallax = 0.25;     // fraction of the monitor width the backdrop can shift
    bool operator==(const BackdropStyle&) const = default;
};

struct StripContent {
    Rect band;                       // overlay coordinates
    std::vector<Rect> tiles;         // overlay coordinates
    std::vector<std::wstring> names;
    Rect plus;                       // empty = no button
    int current = -1;
    float dpiScale = 1.0f;
    bool operator==(const StripContent&) const = default;
};

class SceneRenderer {
public:
    struct Label {
        ComPtr<IDCompositionVisual2> visual;
        ComPtr<IDCompositionVisual3> visual3;
        float w = 0, h = 0;
    };

    bool init(HWND overlay, int width, int height);
    void reset();
    bool ready() const { return device_ != nullptr; }

    // Static content; each call is cheap when its inputs did not change.
    void setBackdrops(const std::vector<BackdropSpec>& specs, const BackdropStyle& style);
    void setStrip(const StripContent& strip);
    Label createLabel(const std::wstring& title, HICON icon, float width, float height, float dpiScale);
    void destroyLabel(Label& label);
    void setFocusChrome(const Rect& cardRect, float dpiScale);  // rasterize ring + shadow for this size

    // Per-frame properties.
    void placeBackdrop(double presence, double transition, int direction);  // transition 0..1 of a desktop slide
    void placeLabel(Label& label, const Rect& cardRect, double opacity, float gap);
    void placeFocus(const Rect& cardRect, double opacity);
    void placeStrip(double presence, double ringX);
    double stripTileX(int index) const;  // ring x for tile `index` (overlay coordinates)
    bool commit();                       // false = device lost; call init() again
    void trim();                         // after the overview closed: release driver scratch memory

private:
    struct Backdrop {
        BackdropSpec spec;
        ComPtr<IDCompositionVisual2> container;               // clipped to the monitor
        ComPtr<IDCompositionVisual2> outgoing, incoming;      // same surface, crossfaded during a slide
        ComPtr<IDCompositionVisual3> outgoing3, incoming3;
        ComPtr<IDCompositionSurface> surface;
        float overscan = 0;  // physical px on each side
    };

    ComPtr<ID2D1Bitmap1> loadImage(const std::wstring& path, UINT w, UINT h, bool cover);
    ComPtr<ID2D1Bitmap1> iconBitmap(HICON icon);
    ComPtr<IDCompositionSurface> beginSurface(UINT w, UINT h, ComPtr<ID2D1DeviceContext>& dc);
    bool endSurface(IDCompositionSurface* s);
    ComPtr<IDCompositionVisual2> visual(IDCompositionVisual2* parent);

    HWND overlay_ = nullptr;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IDCompositionDesktopDevice> device_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> root_, backdropLayer_, focusLayer_, labelLayer_, stripLayer_;

    std::vector<Backdrop> backdrops_;
    BackdropStyle style_;
    ComPtr<IDWriteTextFormat> labelFormat_;
    float labelFormatScale_ = 0;

    ComPtr<IDCompositionVisual2> focus_;
    ComPtr<IDCompositionVisual3> focus3_;
    Rect focusRaster_;   // card size the focus chrome was rasterized for
    float focusMargin_ = 0;

    StripContent strip_;
    ComPtr<IDCompositionVisual2> stripVisual_, stripRing_;
    ComPtr<IDCompositionVisual3> stripVisual3_;
    float stripRingPad_ = 0;
};

}  // namespace mc
