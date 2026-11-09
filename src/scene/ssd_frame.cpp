#include "scene/ssd_frame.h"

#include "config/config.h"
#include "scene/app_icon.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "scene/title_bar_icon.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace umbriel {
  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }

    // «sans 10» → «sans 8». Si el tamaño no se puede leer, se deja igual.
    std::string smallerFont(std::string_view font, int delta) {
      std::string value(font);
      const size_t space = value.find_last_of(' ');
      if (space == std::string::npos || space + 1 >= value.size())
        return value;
      try {
        const int size = std::stoi(value.substr(space + 1));
        return value.substr(0, space + 1) + std::to_string(std::max(6, size - delta));
      } catch (...) {
        return value;
      }
    }
  } // namespace

  SsdFrame::SsdFrame(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent)
      : m_tree(wlr_scene_tree_create(parent)),
        m_gripTree(wlr_scene_tree_create(inputParent != nullptr ? inputParent : parent)), m_labels(std::move(labels)) {
    if (inputParent != nullptr && parent->node.parent == inputParent) {
      // Client surfaces and popups keep input priority over invisible resize grips.
      wlr_scene_node_place_below(&m_gripTree->node, &parent->node);
    }
    const float clear[4]{};
    m_headBorder = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_background = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_controlsBorder = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_controlsBackground = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_close = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_maximize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_minimize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_pin = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_float = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_move = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_fullscreenButton = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_volume = wlr_scene_rect_create(m_tree, 1, 1, clear);
    for (auto*& grip : m_grips)
      grip = wlr_scene_rect_create(m_gripTree, 1, 1, clear);
    wlr_scene_node_set_enabled(&m_tree->node, false);
    wlr_scene_node_set_enabled(&m_gripTree->node, false);
  }

  SsdFrame::~SsdFrame() {
    if (m_blur != nullptr)
      wlr_scene_node_destroy(&m_blur->node);
    if (m_controlsBlur != nullptr)
      wlr_scene_node_destroy(&m_controlsBlur->node);
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
    for (auto* buffer : m_iconBuffers) {
      if (buffer != nullptr)
        wlr_buffer_unlock(buffer);
    }
  }

  void SsdFrame::layout(const ViewChromeGeometry& geometry) {
    if (m_geometry == geometry)
      return;
    m_geometry = geometry;
    redraw();
  }
  void SsdFrame::setFocused(bool focused) {
    if (m_focused == focused)
      return;
    m_focused = focused;
    redraw();
  }
  void SsdFrame::setAlpha(float alpha) { setAlphas(alpha, alpha); }
  void SsdFrame::setOverlayVisible(bool visible) {
    if (m_overlayVisible == visible)
      return;
    m_overlayVisible = visible;
    redraw();
  }
  void SsdFrame::raise() { wlr_scene_node_raise_to_top(&m_tree->node); }
  void SsdFrame::setAlphas(float chromeAlpha, float blurAlpha) {
    chromeAlpha = std::clamp(chromeAlpha, 0.0F, 1.0F);
    blurAlpha = std::clamp(blurAlpha, 0.0F, 1.0F);
    if (m_alpha == chromeAlpha && m_blurAlpha == blurAlpha)
      return;
    m_alpha = chromeAlpha;
    m_blurAlpha = blurAlpha;
    updateColors();
  }
  void SsdFrame::reloadConfig() { redraw(); }
  void SsdFrame::setTitle(std::string_view title) {
    if (m_title == title)
      return;
    m_title = title;
    redraw();
  }
  void SsdFrame::setSubtitle(std::string_view subtitle) {
    if (m_subtitle == subtitle)
      return;
    m_subtitle = subtitle;
    redraw();
  }
  void SsdFrame::setAppId(std::string_view appId) {
    if (m_appId == appId)
      return;
    m_appId = appId;
    m_iconKey.clear();
    redraw();
  }
  void SsdFrame::setResizable(bool resizable) {
    if (m_resizable == resizable)
      return;
    m_resizable = resizable;
    redraw();
  }
  void SsdFrame::setMaximizeEnabled(bool enabled) {
    if (m_maximizeEnabled == enabled)
      return;
    m_maximizeEnabled = enabled;
    updateColors();
  }
  void SsdFrame::setButtons(std::span<const TitleBarButton> buttons) {
    if (std::ranges::equal(m_buttons, buttons))
      return;
    m_buttons.assign(buttons.begin(), buttons.end());
    redraw();
  }
  void SsdFrame::setMaximized(bool maximized) {
    if (m_maximized == maximized)
      return;
    m_maximized = maximized;
    updateColors();
  }
  void SsdFrame::setFullscreen(bool fullscreen) {
    if (m_fullscreen == fullscreen)
      return;
    m_fullscreen = fullscreen;
    updateColors();
  }
  void SsdFrame::setPinned(bool pinned) {
    if (m_pinned == pinned)
      return;
    m_pinned = pinned;
    updateColors();
  }
  void SsdFrame::setFloating(bool floating) {
    if (m_floating == floating)
      return;
    m_floating = floating;
    updateColors();
  }
  void SsdFrame::setHovered(SsdFramePart part) {
    if (m_hovered == part)
      return;
    m_hovered = part;
    updateColors();
  }
  void SsdFrame::setPressed(SsdFramePart part) {
    if (m_pressed == part)
      return;
    m_pressed = part;
    updateColors();
  }

  void SsdFrame::setBlurOptions(const SurfaceBlurOptions& options) {
    m_blurOptions = options;
    updateBlur();
  }
  void SsdFrame::setBlurLive(bool live) {
    m_blurLive = live;
    updateBlur();
  }

  void SsdFrame::updateBlur() {
    const auto& cfg = config().appearance.blur;
    const auto& colors = config().colors.titleBar;
    const float backgroundAlpha = (m_focused ? colors.background : colors.backgroundUnfocused)[3];
    // The title bar has its own transparency, independent of the client's opaque region or alpha mask.
    const bool enabled = m_tree->node.enabled
        && cfg.enabled
        && m_blurOptions.enabled.value_or(false)
        && backgroundAlpha * m_alpha < 1.0F
        && backgroundAlpha >= m_blurOptions.ignoreAlpha;
    const auto sync = [&](wlr_scene_blur*& blur, const wlr_scene_rect* rect, bool shown) {
      if (!enabled || !shown || !rect->node.enabled) {
        if (blur != nullptr)
          wlr_scene_node_set_enabled(&blur->node, false);
        return;
      }
      if (blur == nullptr) {
        // Blur the backdrop below the window, excluding the window's own client content near the title bar seam.
        blur = wlr_scene_blur_create(m_tree->node.parent, rect->width, rect->height);
        if (blur == nullptr)
          return;
        wlr_scene_node_lower_to_bottom(&blur->node);
      }
      wlr_scene_node_set_enabled(&blur->node, true);
      wlr_scene_node_set_position(
          &blur->node, m_tree->node.x + rect->node.x, m_tree->node.y + rect->node.y
      );
      wlr_scene_blur_set_size(blur, rect->width, rect->height);
      wlr_scene_blur_set_corner_radii(blur, rect->corners);
      wlr_scene_blur_set_should_only_blur_bottom_layer(
          blur, m_blurOptions.optimized.value_or(cfg.optimized) && !m_blurLive
      );
      if (blur->strength != m_blurOptions.strength) {
        wlr_scene_blur_set_strength(blur, m_blurOptions.strength);
      }
      wlr_scene_blur_set_alpha(blur, m_blurAlpha);
    };
    sync(m_blur, m_background, true);
    sync(m_controlsBlur, m_controlsBackground, m_geometry.titleBarOverlay);
  }

  int SsdFrame::zoomed(int value) const { return static_cast<int>(std::lround(value * m_geometry.zoom)); }

  std::span<const TitleBarButton> SsdFrame::visibleButtons() const {
    const auto& style = config().appearance.titleBar;
    return style.buttonGlyph != TitleBarButtonGlyph::Never || style.buttonBackground
        ? std::span<const TitleBarButton>(m_buttons)
        : std::span<const TitleBarButton>();
  }

  void SsdFrame::updateButtonIcon(TitleBarButton button, const std::array<float, 4>& color, float alpha, bool visible) {
    const auto index = static_cast<size_t>(button);
    auto*& icon = m_icons[index];
    const std::array<wlr_scene_rect*, kTitleBarButtonCount> backgrounds{
        m_minimize, m_maximize, m_close, m_pin, m_float, m_move, m_fullscreenButton, m_volume
    };
    const auto* background = backgrounds[index];
    if (!visible || !background->node.enabled) {
      if (icon != nullptr)
        wlr_scene_node_set_enabled(&icon->node, false);
      return;
    }
    const int size = titleBarButtonDiameter(config().appearance.titleBar.height);
    const float scale = std::max(1.0F, m_geometry.scale);
    // At integer scales the symbol maps 1:1 onto the button. At fractional scales the button's physical size depends
    // on its position, and the renderer copies a buffer within one pixel of that size texel for texel, leaving the
    // symbol half a pixel off center. Twice the resolution makes it resample symmetrically instead.
    const int pixels = static_cast<int>(std::ceil(size * scale)) * (scale == std::floor(scale) ? 1 : 2);
    if (icon == nullptr || m_iconSizes[index] != pixels || m_iconColors[index] != color) {
      wlr_buffer* buffer = renderTitleBarIcon(button, pixels, color);
      if (icon == nullptr) {
        icon = wlr_scene_buffer_create(m_tree, buffer);
        if (icon != nullptr)
          icon->point_accepts_input = rejectInput;
      } else {
        wlr_scene_buffer_set_buffer(icon, buffer);
      }
      if (icon == nullptr) {
        wlr_buffer_drop(buffer);
        return;
      }
      if (auto* previous = std::exchange(m_iconBuffers[index], wlr_buffer_lock(buffer)); previous != nullptr)
        wlr_buffer_unlock(previous);
      wlr_buffer_drop(buffer);
      m_iconSizes[index] = pixels;
      m_iconColors[index] = color;
    }
    wlr_scene_buffer_set_dest_size(icon, background->width, background->height);
    wlr_scene_buffer_set_opacity(icon, alpha);
    wlr_scene_node_set_position(&icon->node, background->node.x, background->node.y);
    wlr_scene_node_set_enabled(&icon->node, true);
  }

  void SsdFrame::updateColors() {
    const auto& colors = config().colors.titleBar;
    const auto& style = config().appearance.titleBar;
    const bool backgrounds = style.buttonBackground;
    float color[4];
    const auto& background = m_focused ? colors.background : colors.backgroundUnfocused;
    premultiplied(color, background, m_alpha);
    wlr_scene_rect_set_color(m_background, color);
    wlr_scene_rect_set_color(m_controlsBackground, color);
    premultiplied(color, config().colors.border.unfocused, m_alpha);
    wlr_scene_rect_set_color(m_headBorder, color);
    wlr_scene_rect_set_color(m_controlsBorder, color);
    const float contrast =
        background[0] * 0.2126F + background[1] * 0.7152F + background[2] * 0.0722F < 0.5F ? 1.0F : 0.0F;
    // Como en el mockup: sin fondo en reposo y un tinte suave al pasar el
    // ratón. Cerrar usa su rojo; los demás, blanco en oscuro y negro en claro.
    // El estado activo (fijada, flotante, maximizada) usa `.wc.active`: fondo
    // de acento al 13 % y glifo de acento.
    const auto& accent = config().colors.accentPrimary;
    const auto button = [&](wlr_scene_rect* rect, bool danger, const std::array<float, 4>& idleGlyph,
                            const std::array<float, 4>& hoverGlyph, SsdFramePart part, bool enabled,
                            bool active, TitleBarButton which) {
      const bool hovered = m_hovered == part;
      const bool pressed = m_pressed == part;
      std::array<float, 4> fill{contrast, contrast, contrast, 1.0F};
      if (danger) {
        fill[0] = colors.close[0];
        fill[1] = colors.close[1];
        fill[2] = colors.close[2];
      }
      float opacity = 0.0F;
      if (active && enabled) {
        fill = accent;
        opacity = (hovered || pressed) ? 0.20F : 0.13F;
      } else if (backgrounds && enabled && (hovered || pressed)) {
        opacity = (danger ? 0.16F : 0.09F) * (pressed ? 1.35F : 1.0F);
      }
      premultiplied(color, fill, m_alpha * opacity);
      wlr_scene_rect_set_color(rect, color);
      std::array<float, 4> glyphColor = idleGlyph;
      if (active && enabled)
        glyphColor = accent;
      else if (enabled && (hovered || pressed))
        glyphColor = hoverGlyph;
      const bool glyphVisible = style.buttonGlyph == TitleBarButtonGlyph::Always
          || (style.buttonGlyph == TitleBarButtonGlyph::Hover && (hovered || pressed));
      updateButtonIcon(which, glyphColor, m_alpha * (enabled ? 1.0F : 0.35F), glyphVisible);
    };
    const auto& idleGlyph = colors.textUnfocused;
    button(
        m_close, true, idleGlyph, colors.close, SsdFramePart::Close, true, false,
        TitleBarButton::Close
    );
    button(
        m_maximize, false, idleGlyph, colors.text, SsdFramePart::Maximize, m_maximizeEnabled,
        m_maximized, TitleBarButton::Maximize
    );
    button(
        m_minimize, false, idleGlyph, colors.text, SsdFramePart::Minimize, true, false,
        TitleBarButton::Minimize
    );
    button(
        m_float, false, idleGlyph, colors.text, SsdFramePart::Float, true, m_floating,
        TitleBarButton::Float
    );
    button(
        m_pin, false, idleGlyph, colors.text, SsdFramePart::Pin, true, m_pinned,
        TitleBarButton::Pin
    );
    button(
        m_move, false, idleGlyph, colors.text, SsdFramePart::Move, true, false,
        TitleBarButton::Move
    );
    button(
        m_fullscreenButton, false, idleGlyph, colors.text, SsdFramePart::Fullscreen, true,
        m_fullscreen, TitleBarButton::Fullscreen
    );
    button(
        m_volume, false, idleGlyph, colors.text, SsdFramePart::Volume, true, false,
        TitleBarButton::Volume
    );
    if (m_label != nullptr)
      wlr_scene_buffer_set_opacity(m_label, m_alpha);
    if (m_subtitleLabel != nullptr)
      wlr_scene_buffer_set_opacity(m_subtitleLabel, m_alpha);
    updateBlur();
  }

  void SsdFrame::redraw() {
    const auto& g = m_geometry;
    const int h = g.titleBarHeight;
    const bool overlay = g.titleBarOverlay;
    const bool available = !g.suppressed && g.contentWidth > 0 && g.contentHeight > 0;
    const bool shown = available && h > 0 && (!overlay || m_overlayVisible);
    wlr_scene_node_set_enabled(&m_tree->node, shown);
    wlr_scene_node_set_enabled(&m_gripTree->node, available && m_resizable && !m_preview);
    if (!available) {
      updateBlur();
      return;
    }
    wlr_scene_node_set_position(&m_tree->node, zoomed(g.contentX), zoomed(g.contentY));
    wlr_scene_node_set_position(&m_gripTree->node, zoomed(g.contentX), zoomed(g.contentY));
    const auto place = [&](wlr_scene_rect* rect, const wlr_box& box) {
      wlr_scene_node_set_position(&rect->node, zoomed(box.x), zoomed(box.y));
      wlr_scene_rect_set_size(
          rect, std::max(1, zoomed(box.x + box.width) - zoomed(box.x)),
          std::max(1, zoomed(box.y + box.height) - zoomed(box.y))
      );
      rect->accepts_input = !m_preview;
    };
    const int band = g.borderInset + kSsdResizeBand;
    // Overlay chrome lives inside the content, so its top edge is y = 0.
    const int barTop = overlay ? 0 : -h;
    const std::array<wlr_box, 4> grips{{
        {-band, barTop - band, g.contentWidth + 2 * band, band},
        {-band, g.contentHeight, g.contentWidth + 2 * band, band},
        {-band, barTop, band, g.contentHeight - barTop},
        {g.contentWidth, barTop, band, g.contentHeight - barTop},
    }};
    for (size_t i = 0; i < m_grips.size(); ++i) {
      place(m_grips[i], grips[i]);
      wlr_scene_node_set_enabled(&m_grips[i]->node, m_resizable && !m_preview);
    }
    if (!shown) {
      updateBlur();
      return;
    }
    const auto& style = config().appearance.titleBar;
    const int layoutHeight = style.height;
    const int pillHeight = overlay ? layoutHeight : h;
    const int radius = nestedRadius(g.cornerRadius, g.borderInset);
    const TitleBarLayout parts =
        titleBarLayout(g.contentWidth, overlay ? layoutHeight : h, style.buttonPosition, visibleButtons());
    // Floating chrome is two pills: the title on the left and the buttons on
    // the right, each sized to its content instead of spanning the window.
    std::optional<wlr_box> controlsPill;
    int buttonShift = 0;
    if (overlay) {
      const TitleBarControls controls =
          titleBarControls(g.contentWidth, layoutHeight, style.buttonPosition, visibleButtons());
      if (controls.visible) {
        controlsPill = controls.pill;
        buttonShift = controls.shift;
        const wlr_box outer{
            controlsPill->x - 1, controlsPill->y - 1, controlsPill->width + 2, controlsPill->height + 2,
        };
        place(m_controlsBorder, outer);
        wlr_scene_rect_set_corner_radius(m_controlsBorder, zoomed(kOverlayPillRadius + 1));
        wlr_scene_node_set_enabled(&m_controlsBorder->node, true);
        place(m_controlsBackground, *controlsPill);
        wlr_scene_rect_set_corner_radius(m_controlsBackground, zoomed(kOverlayPillRadius));
        wlr_scene_node_set_enabled(&m_controlsBackground->node, true);
      } else {
        wlr_scene_node_set_enabled(&m_controlsBorder->node, false);
        wlr_scene_node_set_enabled(&m_controlsBackground->node, false);
      }
      wlr_scene_node_set_enabled(&m_headBorder->node, true);
      wlr_scene_node_set_enabled(&m_background->node, true);
    } else {
      place(m_background, {0, barTop, g.contentWidth, h});
      wlr_scene_rect_set_corner_radii(m_background, corner_radii_new(zoomed(radius), zoomed(radius), 0, 0));
      wlr_scene_node_set_enabled(&m_background->node, true);
      wlr_scene_node_set_enabled(&m_headBorder->node, false);
      wlr_scene_node_set_enabled(&m_controlsBorder->node, false);
      wlr_scene_node_set_enabled(&m_controlsBackground->node, false);
    }
    const auto drawButton = [&](wlr_scene_rect* button, const std::optional<wlr_box>& slot) {
      if (!slot) {
        wlr_scene_node_set_enabled(&button->node, false);
        return;
      }
      wlr_box box = *slot;
      box.x += buttonShift;
      box.y += overlay ? kOverlayPillMargin : barTop;
      place(button, box);
      const int buttonRadius = style.buttonCornerRadius >= 0 ? style.buttonCornerRadius : g.cornerRadius;
      wlr_scene_rect_set_corner_radius(button, std::min(zoomed(box.width) / 2, zoomed(buttonRadius)));
      wlr_scene_node_set_enabled(&button->node, box.x >= 0 && box.x + box.width <= g.contentWidth);
    };
    drawButton(m_close, parts.button(TitleBarButton::Close));
    drawButton(m_maximize, parts.button(TitleBarButton::Maximize));
    drawButton(m_minimize, parts.button(TitleBarButton::Minimize));
    drawButton(m_pin, parts.button(TitleBarButton::Pin));
    drawButton(m_float, parts.button(TitleBarButton::Float));
    drawButton(m_move, parts.button(TitleBarButton::Move));
    drawButton(m_fullscreenButton, parts.button(TitleBarButton::Fullscreen));
    drawButton(m_volume, parts.button(TitleBarButton::Volume));
    const auto& colors = config().colors.titleBar;
    // Icono real de la app, a la izquierda del título.
    const int iconSize = std::clamp((overlay ? layoutHeight : h) - 16, 12, 20);
    if (m_appId.empty()) {
      if (m_appIcon != nullptr) {
        wlr_scene_node_destroy(&m_appIcon->node);
        m_appIcon = nullptr;
      }
      m_iconKey.clear();
    } else {
      const std::string key =
          m_appId + "@" + std::to_string(iconSize) + "x" + std::to_string(g.scale);
      if (key != m_iconKey) {
        m_iconKey = key;
        if (m_appIcon != nullptr) {
          wlr_scene_node_destroy(&m_appIcon->node);
          m_appIcon = nullptr;
        }
        if (wlr_buffer* buffer = renderAppIcon(m_appId, zoomed(iconSize)); buffer != nullptr) {
          m_appIcon = wlr_scene_buffer_create(m_tree, buffer);
          if (m_appIcon != nullptr)
            m_appIcon->point_accepts_input = rejectInput;
          wlr_buffer_drop(buffer);
        }
      }
    }

    // El mockup pone el subtítulo a la derecha del título, en la misma línea.
    // Flotante: la píldora del título solo ocupa lo que mide su contenido.
    int headX = kOverlayPillMargin;
    int headLimit = g.contentWidth - kOverlayPillMargin;
    if (controlsPill) {
      // With buttons on the left the title pill takes the remaining space.
      if (style.buttonPosition == TitleBarButtonSide::Left)
        headX = controlsPill->x + controlsPill->width + kOverlayPillGap;
      else
        headLimit = controlsPill->x - kOverlayPillGap;
    }
    int titleX = overlay ? headX + kOverlayPillPadding : parts.title.x;
    int titleWidth = overlay
        ? std::max(0, headLimit - titleX - kOverlayPillPadding)
        : parts.title.width;
    const int iconX = titleX;
    if (m_appIcon != nullptr) {
      titleX += iconSize + 6;
      titleWidth = std::max(0, titleWidth - iconSize - 6);
    }
    const int titleEnd = overlay ? headLimit : parts.title.x + parts.title.width;

    const std::array<float, 4> titleColor =
        m_focused ? colors.text : colors.textUnfocused;
    const double labelScale = std::max(1.0, std::ceil(static_cast<double>(g.scale)));

    const TabLabel* subtitle = nullptr;
    if (!overlay && !m_subtitle.empty()) {
      std::array<float, 4> subtitleColor = titleColor;
      subtitleColor[3] *= 0.62F;
      subtitle = m_labels->label({
          .text = m_subtitle,
          .color = subtitleColor,
          .font = smallerFont(style.font, 1),
          .maxWidth = std::max(0, titleWidth / 2),
          .scale = labelScale,
      });
    }
    const int subtitleWidth = subtitle != nullptr ? subtitle->width : 0;
    const int titleMaxWidth = subtitle != nullptr
        ? std::max(0, titleWidth - subtitleWidth - 10) : titleWidth;
    const TabLabel* label = m_labels->label({
        .text = m_title,
        .color = titleColor,
        .font = style.font,
        .maxWidth = titleMaxWidth,
        .scale = labelScale,
    });

    if (m_appIcon != nullptr) {
      const int iconPixels = zoomed(iconSize);
      wlr_scene_buffer_set_dest_size(m_appIcon, iconPixels, iconPixels);
      const int iconY = overlay
          ? kOverlayPillMargin + (layoutHeight - iconSize) / 2
          : barTop + (h - iconSize) / 2;
      wlr_scene_node_set_position(&m_appIcon->node, zoomed(iconX), zoomed(iconY));
    }

    if (label == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, label->buffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, label->buffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(label->width)), std::max(1, zoomed(label->height)));
      const int labelY = overlay
          ? kOverlayPillMargin + (layoutHeight - label->height) / 2
          : barTop + (h - label->height) / 2;
      wlr_scene_node_set_position(&m_label->node, zoomed(titleX), zoomed(labelY));
    }

    if (overlay) {
      const int headWidth = std::min(
          std::max(
              1, 2 * kOverlayPillPadding + (m_appIcon != nullptr ? iconSize + 6 : 0)
                  + (label != nullptr ? label->width : 0)
          ),
          std::max(1, headLimit - headX)
      );
      place(m_background, {headX, kOverlayPillMargin, headWidth, pillHeight});
      wlr_scene_rect_set_corner_radius(m_background, zoomed(kOverlayPillRadius));
      place(m_headBorder, {headX - 1, kOverlayPillMargin - 1, headWidth + 2, pillHeight + 2});
      wlr_scene_rect_set_corner_radius(m_headBorder, zoomed(kOverlayPillRadius + 1));
    }

    if (subtitle == nullptr) {
      if (m_subtitleLabel != nullptr) {
        wlr_scene_node_destroy(&m_subtitleLabel->node);
        m_subtitleLabel = nullptr;
      }
    } else {
      if (m_subtitleLabel == nullptr) {
        m_subtitleLabel = wlr_scene_buffer_create(m_tree, subtitle->buffer);
        m_subtitleLabel->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_subtitleLabel->node, &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_subtitleLabel, subtitle->buffer);
      }
      wlr_scene_buffer_set_dest_size(
          m_subtitleLabel, std::max(1, zoomed(subtitle->width)), std::max(1, zoomed(subtitle->height))
      );
      wlr_scene_node_set_position(
          &m_subtitleLabel->node, zoomed(titleEnd - subtitleWidth),
          zoomed(barTop + (h - subtitle->height) / 2)
      );
    }
    updateColors();
  }

  bool SsdFrame::contains(const wlr_scene_node* node) const {
    for (const wlr_scene_node* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == &m_tree->node || current == &m_gripTree->node)
        return true;
    }
    return false;
  }
  SsdFramePoint SsdFrame::pointAt(double x, double y) const {
    if (m_preview || m_geometry.suppressed || (!m_tree->node.enabled && !m_gripTree->node.enabled))
      return {};
    // Overlay bars sit at y = 0..h inside the content, while the shared hit test
    // expects the bar above the content (y = -h..0).
    const double barY = m_geometry.titleBarOverlay && y >= 0 && y < m_geometry.titleBarHeight
        ? y - m_geometry.titleBarHeight : y;
    const auto& style = config().appearance.titleBar;
    const int shift = m_geometry.titleBarOverlay
        ? titleBarControls(m_geometry.contentWidth, style.height, style.buttonPosition, visibleButtons()).shift
        : 0;
    return ssdFramePointAt(
        m_geometry.contentWidth, m_geometry.contentHeight, m_geometry.titleBarHeight, m_geometry.borderInset,
        m_resizable, style.buttonPosition, visibleButtons(), x, barY, style.height, shift
    );
  }

  void SsdFrame::snapshot(wlr_scene_tree* parent) const {
    if (!m_tree->node.enabled)
      return;
    auto* tree = parent;
    if (m_blur != nullptr && m_blur->node.enabled) {
      auto* copy = wlr_scene_blur_create(tree, m_blur->width, m_blur->height);
      if (copy != nullptr) {
        wlr_scene_node_lower_to_bottom(&copy->node);
        wlr_scene_blur_set_corner_radii(copy, m_blur->corners);
        wlr_scene_blur_set_alpha(copy, m_blur->alpha);
        wlr_scene_blur_set_should_only_blur_bottom_layer(copy, m_blur->should_only_blur_bottom_layer);
        wlr_scene_node_set_position(&copy->node, m_blur->node.x, m_blur->node.y);
      }
    }
    if (m_controlsBlur != nullptr && m_controlsBlur->node.enabled) {
      auto* copy = wlr_scene_blur_create(tree, m_controlsBlur->width, m_controlsBlur->height);
      if (copy != nullptr) {
        wlr_scene_node_lower_to_bottom(&copy->node);
        wlr_scene_blur_set_corner_radii(copy, m_controlsBlur->corners);
        wlr_scene_blur_set_alpha(copy, m_controlsBlur->alpha);
        wlr_scene_blur_set_should_only_blur_bottom_layer(copy, m_controlsBlur->should_only_blur_bottom_layer);
        wlr_scene_node_set_position(&copy->node, m_controlsBlur->node.x, m_controlsBlur->node.y);
      }
    }
    for (const auto* source : {
             m_headBorder, m_background, m_controlsBorder, m_controlsBackground, m_close, m_maximize, m_minimize,
             m_pin, m_float, m_move, m_fullscreenButton,
         }) {
      if (!source->node.enabled)
        continue;
      auto* copy = wlr_scene_rect_create(tree, source->width, source->height, source->color);
      copy->accepts_input = false;
      wlr_scene_rect_set_corner_radii(copy, source->corners);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    }
    const auto copyBuffer = [&](const wlr_scene_buffer* source) {
      if (source == nullptr || !source->node.enabled || source->buffer == nullptr)
        return;
      auto* copy = wlr_scene_buffer_create(tree, source->buffer);
      copy->point_accepts_input = rejectInput;
      wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    };
    copyBuffer(m_label);
    copyBuffer(m_subtitleLabel);
    copyBuffer(m_appIcon);
    for (const auto* icon : m_icons)
      copyBuffer(icon);
  }
  std::unique_ptr<ViewChromeAttachment> SsdFrame::makePreview(wlr_scene_tree* parent) const {
    auto preview = std::make_unique<SsdFrame>(parent, m_labels);
    preview->m_preview = true;
    syncPreview(*preview);
    return preview;
  }
  void SsdFrame::syncPreview(ViewChromeAttachment& preview) const {
    if (auto* frame = dynamic_cast<SsdFrame*>(&preview)) {
      frame->setTitle(m_title);
      frame->setSubtitle(m_subtitle);
      frame->setAppId(m_appId);
      frame->setResizable(false);
      frame->setMaximizeEnabled(m_maximizeEnabled);
      frame->setButtons(m_buttons);
      frame->setMaximized(m_maximized);
      frame->setFullscreen(m_fullscreen);
      frame->setFocused(m_focused);
      frame->setBlurOptions(m_blurOptions);
      frame->reloadConfig();
    }
  }
} // namespace umbriel
