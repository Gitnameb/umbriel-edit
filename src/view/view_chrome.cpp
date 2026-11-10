#include "config/config.h"
#include "core/tracy.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/surface_blur.h"
#include "server/server.h"
#include "view/view.h"
extern "C" {
#include <umbrielfx/render/effect.h>
}
#include "scene/ssd_frame.h"
#include "scene/tab_label_cache.h"
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <algorithm>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  int View::titleBarOverlayChromeHeight() const {
    if (!wantsTitleBar(m_titleBarMode.value_or(config().appearance.titleBar.mode))
        || (m_tiled && m_workspace != nullptr && m_workspace->inTabGroup(this))) {
      return 0;
    }
    const auto& style = config().appearance.titleBar;
    // Floating chrome is two pills inset from every window edge, so the band
    // it draws over the content is taller than the bar itself. It still
    // reserves no space: titleBarHeight() stays zero in overlay mode.
    return style.height + (style.overlay ? 2 * kOverlayPillMargin : 0);
  }

  int View::titleBarChromeHeight() const {
    // A pantalla completa no se reserva ni se dibuja chrome normal. El
    // superpuesto, que solo aparece con Mod mantenido, se calcula aparte.
    if (scheduledFullscreen()) {
      return 0;
    }
    return titleBarOverlayChromeHeight();
  }

  bool View::titleBarOverlay() const {
    return titleBarChromeHeight() > 0 && config().appearance.titleBar.overlay;
  }

  // Controles superpuestos sobre una ventana a pantalla completa. Solo se
  // dibujan mientras se mantiene Mod o el overlay está fijado, y no reservan
  // espacio ni tocan la geometría del juego.
  bool View::fullscreenOverlayChrome() const {
    return (scheduledFullscreen() || currentFullscreen())
        && m_titleBarOverlayVisible
        && config().appearance.titleBar.overlay
        && titleBarOverlayChromeHeight() > 0;
  }

  int View::titleBarHeight() const {
    return titleBarOverlay() ? 0 : titleBarChromeHeight();
  }

  void View::setTitleBarOverlay(bool visible) {
    if (m_titleBarOverlayVisible == visible)
      return;
    m_titleBarOverlayVisible = visible;
    if (visible && fullscreenOverlayChrome()) {
      // Al entrar en pantalla completa la geometría del chrome quedó
      // suprimida. Si Mod se mantiene después, hay que recalcularla antes de
      // mostrarlo; si no, el frame sigue con altura 0 y no se dibuja.
      syncSsdFrame();
      if (m_ssdFrame != nullptr) {
        m_ssdFrame->setOverlayVisible(true);
        m_ssdFrame->raise();
        wlr_scene_node_raise_to_top(&m_attachmentTree->node);
      }
      return;
    }
    if (m_ssdFrame == nullptr) {
      return;
    }
    m_ssdFrame->setOverlayVisible(visible);
    if (visible && titleBarOverlay()) {
      m_ssdFrame->raise();
      wlr_scene_node_raise_to_top(&m_attachmentTree->node);
    }
  }

  void View::setModHandlesActive(bool active) {
    if (m_handles != nullptr) {
      m_handles->setActive(active);
    }
  }

  void View::setTileHandles(
      bool right, double rightStart, double rightEnd, bool bottom, double bottomStart, double bottomEnd
  ) {
    if (m_handles != nullptr) {
      m_handles->setTileEdges(right, rightStart, rightEnd, bottom, bottomStart, bottomEnd);
    }
  }

  void View::setTileHandlesHover(bool right, bool bottom) {
    if (m_handles != nullptr) {
      m_handles->setDividerHover(right, bottom);
    }
  }

  void View::syncWindowHandles(int contentWidth, int contentHeight) {
    if (m_handles == nullptr) {
      return;
    }
    const bool suppressed = !m_mapped || m_server->sessionLocked()
        || (m_server->overview() != nullptr && m_server->overview()->active());
    const bool dotsAllowed = !m_tiled && !m_maximizedToEdges && !floatingMaximized()
        && !scheduledFullscreen() && !currentFullscreen();
    const int gap = m_workspace != nullptr ? m_workspace->layoutConfig().totalGap : config().layoutGap();
    m_handles->layout(contentWidth, contentHeight, gap, m_tiled, dotsAllowed, suppressed);
  }

  bool View::wantsTitleBar(TitleBarMode mode) const {
    if (mode == TitleBarMode::Off)
      return false;
    if (mode == TitleBarMode::Forced)
      return true;
    if (m_xsurface != nullptr)
      return m_xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    return m_xdgDecorationPreference.value_or(m_legacyDecorationPreference.value_or(mode == TitleBarMode::On));
  }

  void View::refreshTitleBar() {
    if (!m_mapped)
      return;
    updateBorderGeometry();
    applyCornerRadius();
    updateBlur();
    updateShadow();
    refitFloatingMaximized();
  }

  void View::setXdgDecorationPreference(std::optional<bool> serverSide) {
    if (m_xdgDecorationPreference == serverSide)
      return;
    m_xdgDecorationPreference = serverSide;
    refreshTitleBar();
  }

  void View::setLegacyDecorationPreference(std::optional<bool> serverSide) {
    if (m_legacyDecorationPreference == serverSide)
      return;
    m_legacyDecorationPreference = serverSide;
    refreshTitleBar();
  }

  void View::onSetDecorations(wl_listener* listener, void*) {
    View* self = wl_container_of(listener, self, m_setDecorations);
    self->handleSetDecorations();
  }

  void View::handleSetDecorations() { refreshTitleBar(); }

  SsdFramePoint View::ssdFramePointAt(double lx, double ly) const {
    if (m_ssdFrame == nullptr)
      return {};
    int x = 0;
    int y = 0;
    if (!wlr_scene_node_coords(&m_contentTree->node, &x, &y))
      return {};
    return m_ssdFrame->pointAt(lx - x, ly - y);
  }

  void View::syncSsdFrame() {
    if (!m_mapped)
      return;
    const int chromeHeight = titleBarChromeHeight();
    const bool fullscreenChrome = fullscreenOverlayChrome();
    // markArrange solo mira el espacio reservado; en overlay (y en pantalla
    // completa) es cero, así que los controles flotantes no reordenan nada.
    const int previousHeight = m_ssdFrame != nullptr
        ? (m_ssdFrame->titleBarOverlayMode() ? 0 : m_ssdFrame->titleBarHeight())
        : 0;
    if (previousHeight != titleBarHeight()) {
      if (m_workspace != nullptr)
        m_workspace->markArrange();
      m_server->scheduleIpcWindowsEvent();
    }
    if ((chromeHeight > 0 || !m_tiled || fullscreenChrome) && m_ssdFrame == nullptr) {
      // Chrome lives in the content tree so its coordinates are the same in
      // both modes: above the content when integrated, over it when floating.
      // Overlay mode raises it above the client surface after each layout.
      m_ssdFrame = std::make_unique<SsdFrame>(chromeParent(), TabLabelCache::shared(), m_sceneTree);
      m_ssdFrame->setOverlayVisible(m_titleBarOverlayVisible);
    }
    if (m_ssdFrame == nullptr)
      return;
    const SizeHints hints = sizeHints();
    const bool fixed = hints.maxWidth > 0
        && hints.maxWidth == hints.minWidth
        && hints.maxHeight > 0
        && hints.maxHeight == hints.minHeight;
    m_ssdFrame->setTitle(title() != nullptr ? title() : "");
    m_ssdFrame->setSubtitle("");
    m_ssdFrame->setAppId(appId() != nullptr ? appId() : "");
    m_ssdFrame->setResizable(!m_tiled);
    m_ssdFrame->setMaximizeEnabled(!fixed);
    syncTitleBarButtons();
    m_ssdFrame->setMaximized(scheduledMaximized() || floatingMaximized());
    m_ssdFrame->setFullscreen(scheduledFullscreen());
    m_ssdFrame->setPinned(pinned());
    m_ssdFrame->setFloating(floating());
    m_ssdFrame->setFocused(m_borderFocusedState);
    m_ssdFrame->setAlphas(chromeAlpha(), blurAlpha());
    m_ssdFrame->setBlurOptions(blurOptions());
    // Honour the window blur mode while dragging too; see enterDragPresentation().
    m_ssdFrame->setBlurLive(false);
    const wlr_box content =
        m_presentedBox.width > 0 && m_presentedBox.height > 0 ? m_presentedBox : committedContentBox();
    layoutChromeAttachment(content.width, content.height);
    // Overlay chrome is raised above client surfaces; attachment dialogs keep
    // the top layer because they are created later in the same tree.
    if (m_ssdFrame != nullptr && m_titleBarOverlayVisible
        && (titleBarOverlay() || fullscreenChrome))
      m_ssdFrame->raise();
    wlr_scene_node_raise_to_top(&m_attachmentTree->node);
  }

  void View::syncTitleBarButtons() {
    if (m_ssdFrame == nullptr)
      return;
    const std::vector<TitleBarButton>& configured =
        m_titleBarButtons ? *m_titleBarButtons : config().appearance.titleBar.buttons;
    // Mismas reglas que el mockup:
    //   · «Flotar» se oculta cuando el escritorio entero es flotante, porque
    //     todas las ventanas ya lo son.
    //   · «Fijar» solo aparece cuando la ventana concreta flota, que es cuando
    //     tiene sentido fijarla.
    const bool workspaceFloating = m_workspace != nullptr
        && m_workspace->layoutMode() == LayoutMode::Stacking;
    std::array<TitleBarButton, kTitleBarButtonCount> shown{};
    size_t count = 0;
    for (const TitleBarButton button : configured) {
      if (button == TitleBarButton::Float && workspaceFloating)
        continue;
      if (button == TitleBarButton::Pin && m_tiled)
        continue;
      // Minimizar también se muestra en mosaico: el clic lo manda al
      // scratchpad, igual que la ficha de la app en el shell.
      if (count < shown.size()) {
        shown[count++] = button;
      }
    }
    m_ssdFrame->setButtons(std::span(shown.data(), count));
  }

  bool View::decorated() const { return m_decoration.bordersVisible(); }

  int View::borderInset() const { return decorated() ? m_decoration.totalBorderWidth() : 0; }

  int View::surfaceRadius() const {
    return decorated() && !scheduledFullscreen() ? nestedRadius(m_decoration.cornerRadius(), borderInset()) : 0;
  }

  void View::setBorderFocused(bool focused) {
    const bool focusChanged = m_borderFocusedState != focused;
    m_borderFocusedState = focused;
    if (m_chromeAttachment != nullptr) {
      m_chromeAttachment->setFocused(focused);
    }
    if (m_ssdFrame != nullptr)
      m_ssdFrame->setFocused(focused);
    if (m_handles != nullptr)
      m_handles->setFocused(focused);

    const auto& animation = config().animation;
    const auto& dim = animation.dimUnfocused;
    if (focusChanged || !m_focusDimInitialized) {
      m_focusDimInitialized = true;
      const double target = focused || !m_mapped || !animation.enabled || !dim.enabled ? 1.0 : 1.0 - dim.dim;
      if (m_mapped && focusChanged && animation.enabled && dim.enabled) {
        m_focusDim.retarget(target, dim.durationMs, dim.curve);
        scheduleFrame();
      } else {
        m_focusDim.snap(target);
      }
      setFadeAlpha(m_fadeAlpha);
    }

    if (focusChanged && m_mapped) {
      applyDynamicRules();
    } else {
      updateBorderColor(focusChanged);
    }
  }

  void View::updateBorderColor(bool focusChanged) {
    const auto& animation = config().animation;
    const Config::Colors::Border& colors = m_decoration.borderColors();
    const auto& targetBase = m_borderFocusedState ? colors.focused : colors.unfocused;
    const auto& border = animation.border;
    const bool borderAnimates = m_mapped && animation.enabled && border.enabled && borderInset() > 0;
    const auto update = [&](AnimatedColor& value, const auto& target, bool focusEvent) {
      if (borderAnimates && (focusChanged || value.animating())) {
        if (value.target() != target || (focusChanged && focusEvent)) {
          value.retarget(target, border.durationMs, border.curve);
          scheduleFrame();
        }
      } else {
        value.snap(target);
      }
    };
    update(m_borderColorAnim, targetBase, true);
    update(m_outerBorderColorAnim, colors.outer, false);
    m_decoration.setBorderRawColors(m_borderColorAnim.current(), m_outerBorderColorAnim.current(), effectiveOpacity());
  }

  void View::settleFocusChrome() {
    if (!m_borderColorAnim.animating()
        && !m_outerBorderColorAnim.animating()
        && !m_focusDim.animating()
        && !m_ruleOpacity.animating()
        && !m_ruleBlur.animating()) {
      return;
    }
    m_borderColorAnim.snap(m_borderColorAnim.target());
    m_outerBorderColorAnim.snap(m_outerBorderColorAnim.target());
    m_focusDim.snap(m_focusDim.target());
    m_ruleOpacity.snap(m_ruleOpacity.target());
    m_ruleBlur.snap(m_ruleBlur.target());
    setFadeAlpha(m_fadeAlpha);
    updateBlur();
    syncAnimationEffects();
  }

  void View::setUrgent(bool urgent) {
    if (m_urgent == urgent) {
      return;
    }
    m_urgent = urgent;
    if (m_workspace != nullptr) {
      m_workspace->updateUrgent();
      // A hidden tab can only ask for attention through its slot in the bar.
      m_workspace->tabs().memberChanged(this);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::applyCornerRadius() {
    // Every buffer under the toplevel's surface tree is rounded against one box, the window's content box, so the arc
    // lands on the window's corners whichever surface (main or subsurface) draws them. Popups are excluded: their
    // surface is its own root.
    const int radius = surfaceRadius();
    // Layout targets, resize animations, and pending floating restores can lead committed geometry. Round the
    // presented frame while the client redraws.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating() || sizeGrabActive() || m_floating.pendingSize().has_value())
        && m_presentation.width() > 0
        && m_presentation.height() > 0;
    struct Ctx {
      View* view;
      int radius;
      int contentWidth;
      int contentHeight;
      int treeX;
      int treeY;
    } ctx{
        this,
        radius,
        usePresentedSize ? m_presentation.width() : geometry.width,
        usePresentedSize ? m_presentation.height() : geometry.height,
        m_contentTree->node.x,
        m_contentTree->node.y,
    };
    wlr_scene_node_for_each_buffer(
        &m_contentTree->node,
        [](wlr_scene_buffer* buffer, int sx, int sy, void* data) {
          auto* ctx = static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr
              || wlr_surface_get_root_surface(sceneSurface->surface) != ctx->view->rootSurface()) {
            return;
          }

          // The iterator accumulates positions from the node it was handed, so subtracting the tree's own position
          // yields tree-local coordinates. The xdg scene helper places the surface tree at (-geometry.x, -geometry.y),
          // which puts the content box at the tree origin, and the corner box is node-relative.
          const wlr_box cornerBox{
              ctx->treeX - sx,
              ctx->treeY - sy,
              ctx->contentWidth,
              ctx->contentHeight,
          };
          // Always set, zeros included: a window that lost its radius must lose the box with it.
          wlr_scene_buffer_set_corner_radii(
              buffer,
              ctx->view->titleBarHeight() > 0 ? corner_radii_new(0, 0, ctx->radius, ctx->radius)
                                              : corner_radii_all(ctx->radius)
          );
          wlr_scene_buffer_set_corner_box(buffer, ctx->radius > 0 ? &cornerBox : nullptr);
        },
        &ctx
    );
  }

  void View::updateBlur() {
    const wlr_box content = committedContentBox();
    updateBlur(content.width, content.height);
  }

  SurfaceBlurOptions View::blurOptions() const {
    auto options = m_decoration.blurOptions();
    // A disabled target must keep drawing until its focus transition reaches zero.
    options.enabled = m_ruleBlur.current() > 0.0 || m_ruleBlur.target() > 0.0;
    return options;
  }

  float View::blurAlpha() const {
    return m_fadeAlpha * m_overviewOpacity * static_cast<float>(std::clamp(m_ruleBlur.current(), 0.0, 1.0));
  }

  void View::updateBlur(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateBlur");
    if (m_ssdFrame != nullptr) {
      m_ssdFrame->setBlurOptions(blurOptions());
      m_ssdFrame->setAlphas(chromeAlpha(), blurAlpha());
    }
    if (currentFullscreen() && fullscreenOpaque()) {
      m_decoration.hideBlur();
      return;
    }
    const wlr_box nodeBox{0, 0, contentWidth, contentHeight};
    const int radius = surfaceRadius();
    // Content and its backdrop share a square seam below the title bar.
    const fx_corner_radii corners =
        titleBarHeight() > 0 ? corner_radii_new(0, 0, radius, radius) : corner_radii_all(radius);
    m_decoration.updateBlur(
        m_contentTree, rootSurface(), nodeBox, geometryBox(), corners, nullptr, effectiveOpacity(), blurAlpha(),
        blurOptions()
    );
  }

  void View::updateShadow() {
    if (scheduledFullscreen() || m_maximizedToEdges) {
      m_decoration.hideShadow();
      return;
    }
    // Shadows follow the presented frame through layout changes, animations, and pending floating restores.
    const wlr_box& geometry = geometryBox();
    const bool usePresentedSize =
        (m_tiled || sizeAnimating() || sizeGrabActive() || m_floating.pendingSize().has_value())
        && m_presentation.width() > 0
        && m_presentation.height() > 0;
    updateShadow(
        usePresentedSize ? m_presentation.width() : geometry.width,
        usePresentedSize ? m_presentation.height() : geometry.height
    );
  }

  void View::updateShadow(int contentWidth, int contentHeight) {
    UMBRIEL_ZONE("View::updateShadow");
    const int borderTotal = borderInset();
    m_decoration.updateShadow(
        contentWidth, contentHeight, borderTotal, decorated() ? m_decoration.cornerRadius() : 0, titleBarHeight()
    );
    m_decoration.setShadowAnimationSource(&m_contentTree->node);
  }

  void View::showDecorations(bool enabled) {
    m_decoration.ensureBorders(m_contentTree);
    m_decoration.setBordersEnabled(enabled);
    updateBorderGeometry();
    applyCornerRadius();
    updateBlur();
    updateShadow();
  }

  void View::updateBorderGeometry() {
    const wlr_box content = committedContentBox();
    updateBorderGeometry(content.width, content.height);
  }

  void View::updateBorderGeometry(int contentWidth, int contentHeight) {
    syncSsdFrame();
    m_decoration.updateBorderGeometry(contentWidth, contentHeight, titleBarHeight());
    layoutChromeAttachment(contentWidth, contentHeight);
  }

  void View::setChromeAttachment(std::unique_ptr<ViewChromeAttachment> attachment) {
    m_chromeAttachment = std::move(attachment);
    if (m_chromeAttachment == nullptr) {
      return;
    }
    m_chromeAttachment->setFocused(m_borderFocusedState);
    m_chromeAttachment->setAlpha(chromeAlpha());
    if (m_chromeContentWidth <= 0 || m_chromeContentHeight <= 0) {
      const wlr_box content = committedContentBox();
      layoutChromeAttachment(content.width, content.height);
    } else {
      layoutChromeAttachment(m_chromeContentWidth, m_chromeContentHeight);
    }
    // Chrome joins the frame after the attachment tree, which must stay on top.
    wlr_scene_node_raise_to_top(&m_attachmentTree->node);
  }

  void View::setModHover(bool visible) {
    if (m_modShadeVisible == visible && m_effects.hoverBlur() == visible) {
      return;
    }
    m_modShadeVisible = visible;
    m_effects.setHoverBlur(visible);
    syncModShade();
    syncAnimationEffects();
    scheduleFrame();
  }

  void View::syncModShade() {
    if (m_modShade == nullptr) {
      return;
    }
    const int radius = surfaceRadius();
    wlr_scene_rect_set_size(m_modShade, std::max(0, m_chromeContentWidth), std::max(0, m_chromeContentHeight));
    wlr_scene_node_set_position(&m_modShade->node, 0, 0);
    wlr_scene_rect_set_corner_radii(
        m_modShade, titleBarHeight() > 0 ? corner_radii_new(0, 0, radius, radius) : corner_radii_all(radius)
    );
    wlr_scene_node_set_enabled(&m_modShade->node, m_modShadeVisible);
  }

  void View::layoutChromeAttachment(int contentWidth, int contentHeight) {
    m_chromeContentWidth = contentWidth;
    m_chromeContentHeight = contentHeight;
    if (m_contentTree == nullptr) {
      return;
    }
    const Output* output = currentOutput();
    const bool fullscreen = scheduledFullscreen() || currentFullscreen();
    const bool fullscreenChrome = fullscreenOverlayChrome();
    ViewChromeGeometry geometry{
        .contentX = 0,
        .contentY = 0,
        .contentWidth = contentWidth,
        .contentHeight = contentHeight,
        .borderInset = borderInset(),
        .cornerRadius = m_maximizedToEdges ? 0 : decorationCornerRadius(),
        .titleBarHeight = fullscreenChrome
            ? titleBarOverlayChromeHeight() : titleBarChromeHeight(),
        .titleBarOverlay = fullscreenChrome || titleBarOverlay(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = !m_mapped || (fullscreen && !fullscreenChrome),
    };
    if (m_ssdFrame != nullptr)
      m_ssdFrame->layout(geometry);
    geometry.suppressed = geometry.suppressed || m_maximizedToEdges;
    if (m_chromeAttachment != nullptr)
      m_chromeAttachment->layout(geometry);
    syncModShade();
  }

  void View::resizeFloatingForTitleBar(int oldReserved, int newReserved) {
    if (m_tiled || !m_mapped || oldReserved == newReserved || scheduledFullscreen()
        || m_maximizedToEdges) {
      return;
    }
    if (m_floatingMaximized) {
      refitFloatingMaximized();
      return;
    }
    const int delta = newReserved - oldReserved;
    const auto [width, height] = floatingSize();
    const int targetHeight = std::max(1, height - delta);
    if (targetHeight != height) {
      requestFloatingSize(width, targetHeight);
    }
    setPosition(layoutTargetX(), layoutTargetY() + delta);
  }

  void View::refreshConfigChrome() {
    // Switching between overlay and integrated changes how much space the bar
    // reserves. The workspace must re-arrange, even though the chrome height
    // itself does not change.
    const bool overlayBefore = m_ssdFrame != nullptr && m_ssdFrame->titleBarOverlayMode();
    const int reservedBefore =
        overlayBefore ? 0 : (m_ssdFrame != nullptr ? m_ssdFrame->titleBarHeight() : 0);
    syncSsdFrame();
    if (m_ssdFrame != nullptr)
      m_ssdFrame->reloadConfig();
    if (m_chromeAttachment != nullptr) {
      m_chromeAttachment->reloadConfig();
    }
    m_focusDimInitialized = false;
    // Temporary unfocus would consume pool selections during an unrelated reload.
    setBorderFocused(m_borderFocusedState);
    updateBorderGeometry();
    const int reservedAfter = titleBarHeight();
    if (overlayBefore != titleBarOverlay() || reservedBefore != reservedAfter) {
      if (m_workspace != nullptr)
        m_workspace->markArrange();
      resizeFloatingForTitleBar(reservedBefore, reservedAfter);
      m_server->scheduleIpcWindowsEvent();
    }
    applyCornerRadius();
    applyDynamicRules();
    if (notifyAloneStateChanged() && m_workspace != nullptr) {
      m_workspace->markArrange();
    }
    updateShadow();
    reloadBackdropColor();
    syncModalShade();
  }
} // namespace umbriel
