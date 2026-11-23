#pragma once
#include <windows.h>
#include <dwmapi.h>

#include "core/geometry.h"

namespace mc {

// RAII owner of one DWM thumbnail. Move-only. Must be used on a single thread.
class Thumbnail {
public:
    Thumbnail() = default;
    ~Thumbnail() { reset(); }
    Thumbnail(Thumbnail&& o) noexcept
        : h_(o.h_), source_(o.source_), srcRect_(o.srcRect_), last_(o.last_), applied_(o.applied_) { o.h_ = nullptr; }
    Thumbnail& operator=(Thumbnail&& o) noexcept;
    Thumbnail(const Thumbnail&) = delete;
    Thumbnail& operator=(const Thumbnail&) = delete;

    // `sourceInWindow` empty = whole window. It is in GetWindowRect pixels and rescaled to the
    // thumbnail's source size (they differ for DPI-virtualized windows).
    bool create(HWND dest, HWND source, const RECT& sourceInWindow);
    // `dest` is in destination-window client coordinates. Skips the DWM call (returns true) when the
    // rounded values equal the last applied ones, so a dead source is only detected on a change.
    bool update(const Rect& dest, double opacity);
    void reset();

    explicit operator bool() const { return h_ != nullptr; }
    HWND source() const { return source_; }

private:
    HTHUMBNAIL h_ = nullptr;
    HWND source_ = nullptr;
    RECT srcRect_{};
    DWM_THUMBNAIL_PROPERTIES last_{};  // last properties DWM accepted
    bool applied_ = false;
};

}  // namespace mc
