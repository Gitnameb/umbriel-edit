#include "scene/native_handles.h"

#include "config/config.h"
#include "scene/color.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr int kDotSize = 8;
    constexpr int kDotInset = 6;
    constexpr int kBandWidth = 14;
    constexpr int kCoreWidth = 2;
    constexpr float kBandAlpha = 0.10F;
    constexpr float kCoreAlpha = 0.42F;
    constexpr float kHoverBandAlpha = 0.18F;
    constexpr float kHoverCoreAlpha = 0.90F;
    constexpr float kDotAlpha = 0.90F;

    wlr_scene_rect* makeRect(wlr_scene_tree* tree) {
      float color[4]{};
      wlr_scene_rect* rect = wlr_scene_rect_create(tree, 1, 1, color);
      if (rect != nullptr) {
        rect->accepts_input = false;
        wlr_scene_node_set_enabled(&rect->node, false);
      }
      return rect;
    }

    // Tramo visible de un borde compartido, con el mismo margen que el diseño:
    // 7 % de cada extremo, acotado entre 6 y 46 px.
    bool spanBounds(double start, double end, int extent, int& first, int& last) {
      if (extent <= 0 || end <= start) {
        return false;
      }
      const int rawStart = std::clamp(static_cast<int>(std::lround(start * extent)), 0, extent);
      const int rawEnd = std::clamp(static_cast<int>(std::lround(end * extent)), 0, extent);
      const int span = rawEnd - rawStart;
      if (span < 48) {
        return false;
      }
      const int inset = std::clamp(static_cast<int>(std::lround(span * 0.07)), 6, 46);
      first = rawStart + inset;
      last = rawEnd - inset;
      return last - first >= 12;
    }
  } // namespace

  NativeHandles::NativeHandles(wlr_scene_tree* parent) : m_parent(parent) {}

  NativeHandles::~NativeHandles() {
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
  }

  void NativeHandles::ensureScene() {
    if (m_tree != nullptr) {
      return;
    }
    m_tree = wlr_scene_tree_create(m_parent);
    for (wlr_scene_rect*& dot : m_dots) {
      dot = makeRect(m_tree);
      if (dot != nullptr) {
        wlr_scene_rect_set_corner_radius(dot, kDotSize / 2);
      }
    }
    m_rightBand = makeRect(m_tree);
    m_rightCore = makeRect(m_tree);
    m_bottomBand = makeRect(m_tree);
    m_bottomCore = makeRect(m_tree);
    if (m_rightBand != nullptr) {
      wlr_scene_rect_set_corner_radius(m_rightBand, kBandWidth / 2);
    }
    if (m_bottomBand != nullptr) {
      wlr_scene_rect_set_corner_radius(m_bottomBand, kBandWidth / 2);
    }
    if (m_rightCore != nullptr) {
      wlr_scene_rect_set_corner_radius(m_rightCore, kCoreWidth / 2);
    }
    if (m_bottomCore != nullptr) {
      wlr_scene_rect_set_corner_radius(m_bottomCore, kCoreWidth / 2);
    }
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void NativeHandles::setActive(bool active) {
    if (m_active == active) {
      return;
    }
    m_active = active;
    apply();
  }

  void NativeHandles::setFocused(bool focused) {
    if (m_focused == focused) {
      return;
    }
    m_focused = focused;
    apply();
  }

  void NativeHandles::setDividerHover(bool right, bool bottom) {
    if (m_hoverRight == right && m_hoverBottom == bottom) {
      return;
    }
    m_hoverRight = right;
    m_hoverBottom = bottom;
    apply();
  }

  void NativeHandles::setTileEdges(
      bool right, double rightStart, double rightEnd, bool bottom, double bottomStart, double bottomEnd
  ) {
    m_right = right;
    m_rightStart = rightStart;
    m_rightEnd = rightEnd;
    m_bottom = bottom;
    m_bottomStart = bottomStart;
    m_bottomEnd = bottomEnd;
    apply();
  }

  void NativeHandles::layout(
      int contentWidth, int contentHeight, int gap, bool tiled, bool dotsAllowed, bool suppressed
  ) {
    m_width = contentWidth;
    m_height = contentHeight;
    m_gap = gap > 0 ? gap : config().layoutGap();
    m_tiled = tiled;
    m_dotsAllowed = dotsAllowed;
    m_suppressed = suppressed;
    apply();
  }

  void NativeHandles::apply() {
    if (!m_active || m_suppressed || m_width <= 0 || m_height <= 0) {
      if (m_visible) {
        wlr_scene_node_set_enabled(&m_tree->node, false);
        m_visible = false;
      }
      return;
    }

    ensureScene();
    if (m_tree == nullptr) {
      return;
    }

    const bool showDots = !m_tiled && m_focused && m_dotsAllowed
        && m_width >= 2 * (kDotInset + kDotSize) && m_height >= 2 * (kDotInset + kDotSize);
    int rightFirst = 0;
    int rightLast = 0;
    int bottomFirst = 0;
    int bottomLast = 0;
    const bool showRight = m_tiled && m_right && spanBounds(m_rightStart, m_rightEnd, m_height, rightFirst, rightLast);
    const bool showBottom = m_tiled && m_bottom
        && spanBounds(m_bottomStart, m_bottomEnd, m_width, bottomFirst, bottomLast);

    const bool visible = showDots || showRight || showBottom;
    if (!visible) {
      if (m_visible) {
        wlr_scene_node_set_enabled(&m_tree->node, false);
        m_visible = false;
      }
      return;
    }

    if (!m_visible) {
      wlr_scene_node_set_enabled(&m_tree->node, true);
      wlr_scene_node_raise_to_top(&m_tree->node);
      m_visible = true;
    }

    // El acento sigue al tema sin reiniciar. Los nodos son pocos y esto solo
    // se ejecuta mientras los indicadores están visibles.
    const auto& accent = config().colors.accentPrimary;
    float bandColor[4]{};
    float coreColor[4]{};
    float dotColor[4]{};
    premultiplied(bandColor, accent, kBandAlpha);
    premultiplied(coreColor, accent, kCoreAlpha);
    premultiplied(dotColor, accent, kDotAlpha);
    float hoverBandColor[4]{};
    float hoverCoreColor[4]{};
    premultiplied(hoverBandColor, accent, kHoverBandAlpha);
    premultiplied(hoverCoreColor, accent, kHoverCoreAlpha);

    for (size_t i = 0; i < m_dots.size(); ++i) {
      wlr_scene_rect* dot = m_dots[i];
      if (dot == nullptr) {
        continue;
      }
      wlr_scene_node_set_enabled(&dot->node, showDots);
      if (!showDots) {
        continue;
      }
      const int x = (i % 2 == 0) ? kDotInset : m_width - kDotInset - kDotSize;
      const int y = (i < 2) ? kDotInset : m_height - kDotInset - kDotSize;
      wlr_scene_node_set_position(&dot->node, x, y);
      wlr_scene_rect_set_size(dot, kDotSize, kDotSize);
      wlr_scene_rect_set_color(dot, dotColor);
    }

    setDividerVisible();
    if (showRight) {
      placeDivider(m_rightBand, m_rightCore, true, m_width + m_gap / 2, rightFirst, rightLast);
      wlr_scene_rect_set_color(m_rightBand, m_hoverRight ? hoverBandColor : bandColor);
      wlr_scene_rect_set_color(m_rightCore, m_hoverRight ? hoverCoreColor : coreColor);
    }
    if (showBottom) {
      placeDivider(m_bottomBand, m_bottomCore, false, m_height + m_gap / 2, bottomFirst, bottomLast);
      wlr_scene_rect_set_color(m_bottomBand, m_hoverBottom ? hoverBandColor : bandColor);
      wlr_scene_rect_set_color(m_bottomCore, m_hoverBottom ? hoverCoreColor : coreColor);
    }
  }

  void NativeHandles::setDividerVisible() {
    for (wlr_scene_rect* rect : {m_rightBand, m_rightCore, m_bottomBand, m_bottomCore}) {
      if (rect != nullptr) {
        wlr_scene_node_set_enabled(&rect->node, false);
      }
    }
  }

  void NativeHandles::placeDivider(
      wlr_scene_rect* band, wlr_scene_rect* core, bool vertical, int position, int first, int last
  ) {
    const int length = last - first;
    if (band == nullptr || core == nullptr || length <= 0) {
      return;
    }
    if (vertical) {
      wlr_scene_node_set_position(&band->node, position - kBandWidth / 2, first);
      wlr_scene_rect_set_size(band, kBandWidth, length);
      wlr_scene_node_set_position(&core->node, position - kCoreWidth / 2, first);
      wlr_scene_rect_set_size(core, kCoreWidth, length);
    } else {
      wlr_scene_node_set_position(&band->node, first, position - kBandWidth / 2);
      wlr_scene_rect_set_size(band, length, kBandWidth);
      wlr_scene_node_set_position(&core->node, first, position - kCoreWidth / 2);
      wlr_scene_rect_set_size(core, length, kCoreWidth);
    }
    wlr_scene_node_set_enabled(&band->node, true);
    wlr_scene_node_set_enabled(&core->node, true);
  }

} // namespace umbriel
