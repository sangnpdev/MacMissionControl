#include "preview/thumbnail.h"

#include <algorithm>
#include <cmath>

#include "diagnostics/log.h"

namespace mc {

Thumbnail& Thumbnail::operator=(Thumbnail&& o) noexcept {
    if (this != &o) {
        reset();
        h_ = o.h_; source_ = o.source_; srcRect_ = o.srcRect_; last_ = o.last_; applied_ = o.applied_;
        o.h_ = nullptr;
    }
    return *this;
}

bool Thumbnail::create(HWND dest, HWND source, const RECT& sourceInWindow) {
    reset();
    if (!log::check(DwmRegisterThumbnail(dest, source, &h_), "DwmRegisterThumbnail")) { h_ = nullptr; return false; }
    source_ = source;
    srcRect_ = {};
    applied_ = false;
    if (!IsRectEmpty(&sourceInWindow)) {
        SIZE thumbSize{};
        RECT wr{};
        if (SUCCEEDED(DwmQueryThumbnailSourceSize(h_, &thumbSize)) && GetWindowRect(source, &wr) && wr.right > wr.left && wr.bottom > wr.top) {
            const double sx = double(thumbSize.cx) / (wr.right - wr.left), sy = double(thumbSize.cy) / (wr.bottom - wr.top);
            srcRect_ = {LONG(std::lround(sourceInWindow.left * sx)), LONG(std::lround(sourceInWindow.top * sy)),
                        LONG(std::lround(sourceInWindow.right * sx)), LONG(std::lround(sourceInWindow.bottom * sy))};
        }
    }
    return true;
}

bool Thumbnail::update(const Rect& dest, double opacity) {
    if (!h_) return false;
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = {LONG(std::lround(dest.x)), LONG(std::lround(dest.y)), LONG(std::lround(dest.right())), LONG(std::lround(dest.bottom()))};
    p.opacity = BYTE(std::lround(std::clamp(opacity, 0.0, 1.0) * 255.0));
    p.fVisible = p.opacity > 0 && !dest.empty();
    p.fSourceClientAreaOnly = FALSE;
    if (!IsRectEmpty(&srcRect_)) {
        p.dwFlags |= DWM_TNP_RECTSOURCE;
        p.rcSource = srcRect_;
    }
    // Each call is a synchronous round trip to DWM; skip it when nothing visible changed.
    if (applied_ && EqualRect(&p.rcDestination, &last_.rcDestination) && p.opacity == last_.opacity && p.fVisible == last_.fVisible)
        return true;
    // Fails with E_INVALIDARG/DWM_E_* once the source window is gone; caller handles removal.
    if (FAILED(DwmUpdateThumbnailProperties(h_, &p))) return false;
    last_ = p;
    applied_ = true;
    return true;
}

void Thumbnail::reset() {
    if (h_) {
        const HRESULT hr = DwmUnregisterThumbnail(h_);
        // DWM already dropped the thumbnail if the source window is gone; that failure is expected.
        if (IsWindow(source_)) log::check(hr, "DwmUnregisterThumbnail");
        h_ = nullptr;
    }
}

}  // namespace mc
