#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class TitleBarButtonSide : uint8_t { Left, Right };
  enum class TitleBarButton : uint8_t { Minimize, Maximize, Close, Pin, Float, Move, Fullscreen, Volume };
  inline constexpr size_t kTitleBarButtonCount = 8;
  enum class SsdFramePart : uint8_t {
    None,
    TitleBar,
    Close,
    Maximize,
    Minimize,
    Pin,
    Float,
    Move,
    Fullscreen,
    Volume,
    Resize
  };

  constexpr int kTitleBarButtonGap = 2;
  constexpr int kTitleBarTitlePadding = 8;
  // Flotante: dos píldoras separadas dentro del contenido, con aire alrededor.
  // La franja dibujada mide `height + 2 * kOverlayPillMargin`.
  constexpr int kOverlayPillMargin = 7;
  constexpr int kOverlayPillPadding = 4;
  constexpr int kOverlayPillGap = 6;
  constexpr int kOverlayPillRadius = 10;
  constexpr int kSsdResizeBand = 6;
  constexpr int kSsdResizeCorner = 20;

  struct TitleBarLayout {
    // Indexed by TitleBarButton; empty for a button the bar does not show.
    std::array<std::optional<wlr_box>, kTitleBarButtonCount> buttons{};
    wlr_box title{};
    const std::optional<wlr_box>& button(TitleBarButton which) const { return buttons[static_cast<size_t>(which)]; }
  };
  // Floating chrome: the button pill around the visible slots, and the offset
  // those slots are drawn at so the pill keeps its right or left margin.
  struct TitleBarControls {
    wlr_box pill{};
    int shift = 0;
    bool visible = false;
  };
  struct SsdFramePoint {
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  // About half the bar height, with the bar's parity so the margins above and below are whole and equal pixels.
  int titleBarButtonDiameter(int height);
  // `shown` lists the buttons from the title outward, so the last one sits at the bar's edge on `side`.
  TitleBarLayout titleBarLayout(int width, int height, TitleBarButtonSide side, std::span<const TitleBarButton> shown);
  // Floating pill wrapping the visible buttons, drawn inside `layoutHeight`.
  TitleBarControls titleBarControls(
      int width, int layoutHeight, TitleBarButtonSide side, std::span<const TitleBarButton> shown
  );
  // Coordinates relative to client content; the title bar lies above y = 0.
  // `layoutHeight` is the bar's own height when `h` includes overlay margins;
  // zero means it equals `h`. `buttonShift` moves the button hit zones by the
  // same amount the floating pill moves them.
  SsdFramePoint ssdFramePointAt(
      int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, std::span<const TitleBarButton> shown,
      double x, double y, int layoutHeight = 0, int buttonShift = 0
  );
  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight);
} // namespace umbriel
