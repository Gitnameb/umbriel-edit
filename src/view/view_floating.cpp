#include "config/config.h"
#include "core/log.h"
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
#include "view/size_hints.h"
#include "view/view_internal.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <ranges>
#include <string_view>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {
  namespace {
    constexpr Logger kLog("view");
  }

  wlr_box View::floatingUsableArea() const {
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      return m_workspace->group()->output()->usableArea();
    }
    if (m_server != nullptr && m_server->scratchpadManager() != nullptr) {
      if (Output* output = m_server->scratchpadManager()->outputFor(this)) {
        return output->usableArea();
      }
    }
    return m_server->usableAreaAt(layoutX(), layoutY());
  }

  wlr_box View::floatingMarginArea(const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    if (layout == nullptr && m_workspace != nullptr) {
      layout = &m_workspace->layoutConfig();
    }
    const LayoutStruts& struts = layout != nullptr ? layout->struts : config().layout.struts;
    const int pad = layout != nullptr ? layout->edgePad : config().layoutEdgePad();
    const wlr_box inside = applyLayoutStruts(usable, struts);
    const wlr_box inset{
        .x = inside.x + pad,
        .y = inside.y + pad,
        .width = inside.width - (2 * pad),
        .height = inside.height - (2 * pad),
    };
    return inset.width > 0 && inset.height > 0 ? inset : usable;
  }

  wlr_box View::floatingMaximizedBox(const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    if (usable.width <= 0 || usable.height <= 0) {
      return usable;
    }
    const wlr_box area =
        contentBoxInTile(m_maximizedToEdges ? usable : floatingMarginArea(usable, layout), titleBarHeight());
    // A client whose maximum size is below the area (a fixed-size splash) keeps that size, centered in the area
    // rather than pinned to its corner.
    const SizeHints hints = sizeHints();
    const int width = std::min(area.width, clampWidth(area.width, hints));
    const int height = std::min(area.height, clampHeight(area.height, hints));
    return {
        .x = area.x + ((area.width - width) / 2),
        .y = area.y + ((area.height - height) / 2),
        .width = width,
        .height = height,
    };
  }

  wlr_box View::openingUsableArea(Output* targetOutput) const {
    const wlr_cursor* cursor = m_server->cursor()->wlr();
    if (targetOutput == nullptr) {
      return m_server->usableAreaAt(cursor->x, cursor->y);
    }
    wlr_box usable = targetOutput->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), targetOutput->wlr(), &usable);
    }
    if (usable.width <= 0 || usable.height <= 0) {
      usable = m_server->usableAreaAt(cursor->x, cursor->y);
    }
    return usable;
  }

  FloatingPoint View::grabbableOrigin(FloatingPoint origin, const wlr_box& usable) const {
    return keepTitleBarReachable(clampFloatingOrigin(origin, geometryBox(), usable), titleBarHeight(), usable);
  }

  FloatingPoint View::settleFloatingDrop(int x, int y) {
    const wlr_box usable = floatingUsableArea();
    const FloatingPoint settled = grabbableOrigin({x, y}, usable);
    if (settled.x != x || settled.y != y) {
      animateTo(settled.x, settled.y);
    } else {
      setPosition(x, y);
    }
    rememberFloatingPosition();
    return settled;
  }

  std::optional<FloatingPoint> View::floatingClampTarget(FloatingPoint origin, int width, int height, bool resize) {
    if (m_tiled || !m_mapped || attached() || scheduledFullscreen() || scheduledMaximized() || sizeGrabActive()) {
      return std::nullopt;
    }
    if (Cursor* cursor = m_server->cursor(); cursor != nullptr && cursor->isDraggingView(this)) {
      return std::nullopt;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || width <= 0 || height <= 0) {
      return std::nullopt;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box box{.x = geo.x, .y = geo.y, .width = width, .height = height};
    const FloatingPoint clamped = keepTitleBarReachable(
        resize ? clampFloatingOriginForResize(origin, box, usable) : clampFloatingOrigin(origin, box, usable),
        titleBarHeight(), usable
    );
    if (clamped.x == origin.x && clamped.y == origin.y) {
      return std::nullopt;
    }
    return clamped;
  }

  void View::clampFloatingPosition() {
    // Mid-tween the clamp applies to where the tween is headed and retargets it. Skipping instead would spend the
    // usable-change check on a target that settles out of the margin, and the change is never re-checked.
    const bool animating = m_posX.animating() || m_posY.animating();
    const wlr_box& geo = geometryBox();
    const FloatingPoint origin{
        .x = animating ? layoutTargetX() : layoutX(),
        .y = animating ? layoutTargetY() : layoutY(),
    };
    if (const auto clamped = floatingClampTarget(origin, geo.width, geo.height, false)) {
      if (animating) {
        animateTo(clamped->x, clamped->y);
      } else {
        setPosition(clamped->x, clamped->y);
      }
    }
  }

  void View::clampFloatingPositionForSize(int width, int height) {
    const FloatingPoint origin{.x = layoutTargetX(), .y = layoutTargetY()};
    if (const auto clamped = floatingClampTarget(origin, width, height, true)) {
      animateTo(clamped->x, clamped->y);
    }
  }

  void View::rememberFloatingPosition() {
    if (m_tiled) {
      return;
    }
    m_floating.rememberPositionFraction({layoutTargetX(), layoutTargetY()}, floatingUsableArea());
  }

  void View::restoreFloatingPosition(bool rememberRestored) {
    if (m_tiled) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (const std::optional<FloatingPoint> restored = m_floating.restoredOrigin(usable)) {
      // The remembered fraction is proportional, so on a smaller output it can resolve outside the grabbable margin
      // (a scale jump can leave the window fully off). Placements land inside it, and the clamped origin (where the
      // window actually is) is what gets re-anchored.
      const FloatingPoint origin = grabbableOrigin(*restored, usable);
      animateTo(origin.x, origin.y);
      if (rememberRestored) {
        // Re-anchor on the new usable area so a second deliberate cross-output move lands proportionally again.
        m_floating.rememberPositionFraction(origin, usable);
      }
      return;
    }
    clampFloatingPosition();
  }

  bool View::centerFloating() {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    const wlr_box& geo = geometryBox();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    FloatingPoint origin = centeredOrigin(usable, geo.width, geo.height + titleBarHeight());
    origin.y += titleBarHeight();
    clearRegionState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  bool View::moveFloatingTo(const WindowPosition& position) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }

    const wlr_box usable = floatingUsableArea();

    const auto floatingPosition = getFloatingPosition(usable, position, std::nullopt);
    if (!floatingPosition) {
      return false;
    }
    clearRegionState();
    animateTo(floatingPosition->x, floatingPosition->y);
    m_floating.rememberPositionFraction(*floatingPosition, usable);
    return true;
  }

  bool View::moveFloatingBy(int deltaX, int deltaY) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    FloatingPoint origin = {.x = layoutTargetX() + deltaX, .y = layoutTargetY() + deltaY};
    origin = clampFloatingOrigin(origin, geo, usable);
    clearRegionState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  bool View::moveFloatingToEdge(wlr_edges edge) {
    if (!m_mapped || m_tiled || scheduledFullscreen() || currentFullscreen()) {
      return false;
    }
    const wlr_box& geo = geometryBox();
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0 || geo.width <= 0 || geo.height <= 0) {
      return false;
    }
    const wlr_box area = floatingMarginArea(usable);
    const FloatingPoint current{.x = layoutTargetX(), .y = layoutTargetY()};
    FloatingPoint origin = current;
    switch (edge) {
    case WLR_EDGE_LEFT:
      origin.x = area.x;
      break;
    case WLR_EDGE_RIGHT:
      origin.x = area.x + area.width - geo.width;
      break;
    case WLR_EDGE_TOP:
      origin.y = area.y;
      break;
    case WLR_EDGE_BOTTOM:
      origin.y = area.y + area.height - geo.height;
      break;
    case WLR_EDGE_NONE:
      return false;
    }
    if (origin.x == current.x && origin.y == current.y) {
      return false;
    }
    clearRegionState();
    animateTo(origin.x, origin.y);
    m_floating.rememberPositionFraction(origin, usable);
    return true;
  }

  std::optional<FloatingPoint> View::getFloatingPosition(
      const wlr_box usable, const std::optional<WindowPosition>& position, const std::optional<std::array<int, 2>> size
  ) {
    if (usable.width <= 0 || usable.height <= 0) {
      return std::nullopt;
    }

    int w;
    int h;
    // Use size if provided
    if (size) {
      w = (*size)[0];
      h = (*size)[1];
    } else {
      const wlr_box& geo = geometryBox();
      w = geo.width > 0 ? geo.width : usable.width;
      h = geo.height > 0 ? geo.height : usable.height;
    }
    // Floats keep their own size; only center within the usable area.
    const int width = w;
    const int height = h;
    FloatingPoint origin = centeredOrigin(usable, width, height + titleBarHeight());
    origin.y += titleBarHeight();
    if (position) {
      origin = {.x = usable.x + position->x, .y = usable.y + position->y};
      switch (position->anchor) {
      case WindowPositionAnchor::TopLeft:
        break;
      case WindowPositionAnchor::TopRight:
        origin.x = usable.x + usable.width - width - position->x;
        break;
      case WindowPositionAnchor::BottomLeft:
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::BottomRight:
        origin.x = usable.x + usable.width - width - position->x;
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::Top:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        break;
      case WindowPositionAnchor::Bottom:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        origin.y = usable.y + usable.height - height - position->y;
        break;
      case WindowPositionAnchor::Left:
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      case WindowPositionAnchor::Right:
        origin.x = usable.x + usable.width - width - position->x;
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      case WindowPositionAnchor::Center:
        origin.x = usable.x + (usable.width - width) / 2 + position->x;
        origin.y = usable.y + (usable.height - height) / 2 + position->y;
        break;
      }
      origin = clampFloatingOrigin(origin, {.x = 0, .y = 0, .width = width, .height = height}, usable);
    }
    return keepTitleBarReachable(origin, titleBarHeight(), usable);
  }

  std::array<int, 2>
  View::stackingDefaultFloatingSize(const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    if (usable.width <= 0 || usable.height <= 0) {
      return {0, 0};
    }
    if (layout == nullptr && m_workspace != nullptr) {
      layout = &m_workspace->layoutConfig();
    }
    if (layout == nullptr) {
      return {0, 0};
    }
    const wlr_box area = floatingMarginArea(usable, layout);
    const int availableHeight = std::max(1, area.height - titleBarHeight());
    const SizeHints hints = sizeHints();
    const int width = clampWidth(floatingFractionSize(layout->stacking.defaultWidthFraction, usable.width), hints);
    const int height = clampHeight(floatingFractionSize(layout->stacking.defaultHeightFraction, usable.height), hints);
    return {width, std::min(height, availableHeight)};
  }

  void View::placeInUsableArea(
      const std::optional<WindowPosition>& position, std::optional<std::array<int, 2>> placementSize,
      bool openingPlacement
  ) {
    if (attached()) {
      return;
    }
    const wlr_box usable = floatingUsableArea();

    const bool stackingWorkspace =
        m_workspace != nullptr && m_workspace->layoutMode() == LayoutMode::Stacking;
    const bool defaultSizeEligible = !placementSize
        && transientParent() == nullptr
        && !passiveXwaylandUtility()
        && stackingWorkspace
        && !m_initialRules.defaultFloatingWidth
        && !m_initialRules.defaultFloatingWidthPx
        && !m_initialRules.defaultFloatingHeight
        && !m_initialRules.defaultFloatingHeightPx
        && !scheduledFullscreen()
        && !scheduledMaximized();
    if (defaultSizeEligible && openingPlacement && !m_floating.size()) {
      // A window that opens on a stacking workspace has not floated before, so
      // it takes the same default size a tiled window gets when the workspace
      // switches to cascading. The cascade below then puts it in the next
      // diagonal slot. The size leaves room for the title bar and layout edges.
      const auto [width, height] = stackingDefaultFloatingSize(usable);
      if (width > 0 && height > 0) {
        placementSize = std::array{width, height};
        requestFloatingSize(width, height);
      }
    } else if (defaultSizeEligible && !position) {
      const auto [width, height] = floatingSize();
      const wlr_box area = floatingMarginArea(usable);
      const int availableHeight = std::max(1, area.height - titleBarHeight());
      if (width > 0 && height > 0 && (width > area.width || height > availableHeight)) {
        const double scale =
            std::min(static_cast<double>(area.width) / width, static_cast<double>(availableHeight) / height);
        const SizeHints hints = sizeHints();
        placementSize = std::array{
            clampWidth(static_cast<int>(width * scale), hints), clampHeight(static_cast<int>(height * scale), hints)
        };
        requestFloatingSize((*placementSize)[0], (*placementSize)[1]);
      }
    }
    std::optional<FloatingPoint> origin = getFloatingPosition(usable, position, placementSize);
    if (!origin) {
      return;
    }
    if (position) {
      m_floating.rememberPositionFraction(*origin, usable);
    } else if (passiveXwaylandUtility()) {
      const XwaylandRegion region = m_server->xwayland()->outputs().regionAtX(m_xsurface->x, m_xsurface->y);
      setPosition(region.toLayoutX(m_xsurface->x), region.toLayoutY(m_xsurface->y));
      return;
    } else if (const View* parent = transientParent()) {
      // Where the parent is headed, not where its node is mid-animation right after it mapped. A fullscreen parent
      // shows over any panel, so the whole output counts as visible for it.
      const wlr_box shownIn = parent->scheduledFullscreen() ? parent->fullscreenArea() : usable;
      const wlr_box& geo = geometryBox();
      const auto size = placementSize.value_or(
          std::array{geo.width > 0 ? geo.width : usable.width, geo.height > 0 ? geo.height : usable.height}
      );
      origin = centeredOverShown(parent->targetBox(), shownIn, size[0], size[1]);
    } else if (m_workspace != nullptr && m_workspace->layoutMode() == LayoutMode::Stacking) {
      const auto [width, height] = placementSize.value_or(floatingSize());
      origin = cascadeFloatingPlacement(*origin, width, height);
    }
    *origin = keepTitleBarReachable(*origin, titleBarHeight(), usable);
    setPosition(origin->x, origin->y);
  }

  FloatingPoint View::cascadeFloatingPlacement(FloatingPoint origin, int width, int height) const {
    if (m_workspace == nullptr) {
      return origin;
    }
    const wlr_box usable = floatingUsableArea();
    const int border = borderInset();
    const int bar = titleBarHeight();
    int step = floatingCascadeStep(bar, border);
    if (const int configured = m_workspace->layoutConfig().stacking.cascadeStep; configured > 0) {
      step = std::max(step, configured);
    }
    std::vector<wlr_box> taken;
    const auto& stack = m_workspace->floatingStack();
    for (const View* other : std::views::reverse(stack)) {
      if (other != this
          && other->mapped()
          && other->floating()
          && other->positioned()
          && !other->pinned()
          && !other->scheduledFullscreen()) {
        const int otherBorder = other->borderInset();
        const int otherBar = other->titleBarHeight();
        const auto [otherWidth, otherHeight] = other->floatingSize();
        taken.push_back({
            .x = other->layoutTargetX() - otherBorder,
            .y = other->layoutTargetY() - otherBar - otherBorder,
            .width = otherWidth + 2 * otherBorder,
            .height = otherHeight + otherBar + 2 * otherBorder,
        });
        step = std::max(step, floatingCascadeStep(otherBar, otherBorder));
      }
    }
    const FloatingPoint frame = cascadeOrigin(
        {.x = origin.x - border, .y = origin.y - bar - border}, taken, step, usable, width + 2 * border,
        height + bar + 2 * border
    );
    return {.x = frame.x + border, .y = frame.y + bar + border};
  }

  void View::requestFloatingSize(int width, int height) {
    if (m_xsurface != nullptr) {
      m_xFloatingRequestBase = currentSize();
    }
    m_floating.recordSizeRequest(width, height, configureSize(width, height));
  }

  // Piloto de render fijo. Solo para Xwayland y escala de salida 1: el cliente
  // conserva su tamaño (su resolución) y la caja flotante se escala. Así una
  // ventana más pequeña no reconfigura al cliente y no dispara los bugs de
  // resize de los juegos.
  void View::beginFixedRenderPilot() {
    if (m_fixedRender || m_tiled || m_xsurface == nullptr || m_contentTree == nullptr) {
      return;
    }
    const std::string_view id = appId() != nullptr ? std::string_view(appId()) : std::string_view{};
    const SizeHints hints = sizeHints();
    const bool fixedSize = hints.maxWidth > 0
        && hints.maxHeight > 0
        && hints.minWidth == hints.maxWidth
        && hints.minHeight == hints.maxHeight;
    // Juegos de Steam y cualquier app que ya pida fullscreen o declare tamaño
    // fijo. El piloto solo escala Xwayland; una app Wayland nativa no entra.
    if (!id.starts_with("steam_app_") && !requestedFullscreen() && !scheduledFullscreen() && !fixedSize) {
      return;
    }
    auto size = currentSize();
    if (size.width <= 0 || size.height <= 0) {
      const wlr_box& geo = geometryBox();
      size = {.width = geo.width, .height = geo.height};
    }
    if (size.width <= 0 || size.height <= 0) {
      return;
    }
    m_fixedRender = true;
    m_fixedRenderWidth = size.width;
    m_fixedRenderHeight = size.height;
    m_fixedBoxWidth = size.width;
    m_fixedBoxHeight = size.height;
    m_fixedScale = 1.0;
    // El cliente no debe recibir el tamaño de la caja, sino su resolución.
    m_xScheduled = {.width = m_fixedRenderWidth, .height = m_fixedRenderHeight};
    syncXwaylandConfigure();
    m_presentation.setSize(m_fixedBoxWidth, m_fixedBoxHeight);
    applyContentScale();
    kLog.info(
        "fixed-render '{}': cliente {}x{}, caja {}x{}", appId(), m_fixedRenderWidth, m_fixedRenderHeight,
        m_fixedBoxWidth, m_fixedBoxHeight
    );
  }

  void View::resizeFixedRenderBox(int width, int height) {
    if (!m_fixedRender || m_fixedRenderWidth <= 0 || m_fixedRenderHeight <= 0) {
      return;
    }
    // La caja puede tener cualquier proporción, como una ventana de vídeo: el
    // contenido se escala con fit y las barras que sobren se pintan en negro.
    const int boxWidth = std::max(1, width);
    const int boxHeight = std::max(1, height);
    m_fixedBoxWidth = boxWidth;
    m_fixedBoxHeight = boxHeight;
    m_fixedScale = std::min(
        static_cast<double>(boxWidth) / m_fixedRenderWidth,
        static_cast<double>(boxHeight) / m_fixedRenderHeight
    );
    kLog.debug(
        "fixed-render '{}': caja {}x{}, escala {:.4}", appId(), boxWidth, boxHeight, m_fixedScale
    );
    m_presentation.setSize(boxWidth, boxHeight);
    beginResizeAnimation(boxWidth, boxHeight);
    applyContentScale();
    // La ventana X cambia de posición con la caja, pero no de tamaño.
    if (const auto& anchor = m_floating.anchor(); anchor && !currentFullscreen()) {
      // Al arrastrar un borde, el opuesto se queda quieto y la caja crece
      // hacia el lado que se mueve.
      const auto origin = anchoredContentOrigin(
          *anchor, m_floating.edges(), {.x = 0, .y = 0, .width = boxWidth, .height = boxHeight}
      );
      setPosition(origin.x, origin.y);
    }
    // Igual que la ruta normal: si la caja nueva no cabe, se recoloca. Durante
    // un arrastre con el ratón esto no hace nada y el cursor manda.
    clampFloatingPositionForSize(boxWidth, boxHeight);
    syncXwaylandConfigure();
    syncOwnedPresentation();
  }

  // El árbol de superficie se dibuja con una escala divisoria: el tamaño en
  // escena es el del búfer dividido por la escala. La escala de salida de
  // Xwayland (m_xScale) ya divide el búfer a tamaño lógico, así que para
  // agrandar el contenido k veces hay que dividir por k: m_xScale / k.
  void View::applyContentScale() {
    if (m_contentTree == nullptr) {
      return;
    }
    const double scale = contentScale();
    wlr_scene_subsurface_tree_set_scale(&m_contentTree->node, scale);
    if (m_captureScene != nullptr) {
      wlr_scene_subsurface_tree_set_scale(&m_captureScene->tree.node, scale);
    }
    if (m_fixedRender) {
      // El centrado del fullscreen anterior pudo mover este nodo; el piloto
      // siempre lo quiere en el origen de m_contentTree y compensa con
      // m_fixedOffsetX/Y.
      if (wlr_scene_node* surfaceNode = view_detail::toplevelSurfaceTreeNode(m_contentTree, rootSurface())) {
        wlr_scene_node_set_position(surfaceNode, 0, 0);
      }
      struct Ctx {
        double applied;
      } ctx{scale};
      wlr_scene_node_for_each_buffer(
          &m_contentTree->node,
          [](wlr_scene_buffer* buffer, int sx, int sy, void* data) {
            auto* ctx = static_cast<Ctx*>(data);
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface == nullptr) {
              return;
            }
            int width = 0;
            int height = 0;
            wlr_scene_surface_get_size(sceneSurface, &width, &height);
            kLog.debug(
                "fixed-render buffer '{}x{}' at {},{}, dest {}x{}, divisor {:.4}", sceneSurface->surface->current.width,
                sceneSurface->surface->current.height, sx, sy, width, height, ctx->applied
            );
          },
          &ctx
      );
      int lx = 0;
      int ly = 0;
      wlr_scene_node_coords(&m_contentTree->node, &lx, &ly);
      kLog.debug("fixed-render contenido en {},{} divisor {:.4}", lx, ly, scale);
    }
    syncXwaylandShape();
  }

  std::array<int, 2> View::floatingSize() const {
    if (m_fixedRender && !m_tiled && m_fixedBoxWidth > 0 && m_fixedBoxHeight > 0) {
      return {m_fixedBoxWidth, m_fixedBoxHeight};
    }
    if (const auto& pending = m_floating.pendingSize()) {
      return *pending;
    }
    const wlr_box& geo = geometryBox();
    return {geo.width, geo.height};
  }

  std::optional<std::array<int, 2>> View::floatingAxisBasis(bool width) const {
    if (!m_mapped || m_tiled) {
      return std::nullopt;
    }
    const wlr_box usable = floatingUsableArea();
    const auto [basisWidth, basisHeight] = floatingSize();
    const int basis = width ? basisWidth : basisHeight;
    const int extent = width ? usable.width : usable.height;
    if (extent <= 0 || basis <= 0) {
      return std::nullopt;
    }
    return std::array{basis, extent};
  }

  std::optional<double> View::floatingFraction(bool width) const {
    const auto axis = floatingAxisBasis(width);
    if (!axis) {
      return std::nullopt;
    }
    return floatingSizeFraction((*axis)[0], (*axis)[1]);
  }

  bool View::resizeFloatingFractions(
      const std::optional<double>& widthFraction, const std::optional<double>& heightFraction
  ) {
    if (!m_mapped || m_tiled || currentFullscreen() || scheduledFullscreen()) {
      return false;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return false;
    }
    const SizeHints hints = sizeHints();
    const auto [basisWidth, basisHeight] = floatingSize();
    // El piloto de render fijo desacopla la caja del cliente: sus límites de
    // tamaño son la resolución del juego, no los de la ventana.
    const int width = widthFraction
        ? (m_fixedRender ? floatingFractionSize(*widthFraction, usable.width)
                         : clampWidth(floatingFractionSize(*widthFraction, usable.width), hints))
        : basisWidth;
    const int height = heightFraction
        ? (m_fixedRender ? floatingFractionSize(*heightFraction, usable.height)
                         : clampHeight(floatingFractionSize(*heightFraction, usable.height), hints))
        : basisHeight;
    if (width <= 0 || height <= 0) {
      return false;
    }
    clearRegionState();
    dropMaximizedForResize();
    if (m_fixedRender) {
      // Un resize por teclado crece desde el borde superior izquierdo; se
      // descarta cualquier ancla de un arrastre anterior.
      finishFloatingResize();
      resizeFixedRenderBox(width, height);
      return true;
    }
    requestFloatingSize(width, height);
    beginResizeAnimation(width, height);
    clampFloatingPositionForSize(width, height);
    return true;
  }

  std::array<int, 2> View::floatingRestoreSize() const {
    if (m_fixedRender && m_fixedBoxWidth > 0 && m_fixedBoxHeight > 0) {
      return {m_fixedBoxWidth, m_fixedBoxHeight};
    }
    if (m_floating.size()) {
      return *m_floating.size();
    }
    // A window that has never floated inherits its tiled size today. Alone in
    // a tile that size is the whole output, so floating it would stay
    // full-screen. Give the first float episode the configured default and let
    // the remembered size take over once the user resizes it.
    if (m_tiled && m_workspace != nullptr) {
      const ResolvedLayoutConfig::Stacking& stacking = m_workspace->layoutConfig().stacking;
      const wlr_box usable = floatingUsableArea();
      if (usable.width > 0 && usable.height > 0) {
        const SizeHints hints = sizeHints();
        const int width = clampWidth(floatingFractionSize(stacking.defaultWidthFraction, usable.width), hints);
        const int height = clampHeight(floatingFractionSize(stacking.defaultHeightFraction, usable.height), hints);
        if (width > 0 && height > 0) {
          return {width, height};
        }
      }
    }
    // First-time floats prefer the last acked or scheduled configure size,
    // then fall back to the layout target and committed geometry.
    int width = currentSize().width;
    int height = currentSize().height;
    if (width <= 0 || height <= 0) {
      width = scheduledSize().width;
      height = scheduledSize().height;
    }
    if ((width <= 0 || height <= 0) && m_workspace != nullptr) {
      const wlr_box target = m_workspace->layout().targetBox(this);
      if (target.width > 0 && target.height > 0) {
        const SizeHints hints = sizeHints();
        width = clampWidth(target.width, hints);
        height = clampHeight(target.height, hints);
      }
    }
    if (width <= 0 || height <= 0) {
      const wlr_box& geo = geometryBox();
      width = geo.width;
      height = geo.height;
    }
    return {width, height};
  }

  void View::beginFloatingResize(uint32_t edges) {
    clearRegionState();
    if (m_fixedRender) {
      // La caja, no el tamaño del cliente, es lo que se ancla al arrastrar.
      m_floating.beginResize({layoutX(), layoutY(), m_fixedBoxWidth, m_fixedBoxHeight}, edges);
      return;
    }
    const wlr_box& geo = geometryBox();
    m_floating.beginResize(
        {.x = layoutX() + geo.x, .y = layoutY() + geo.y, .width = geo.width, .height = geo.height}, edges
    );
    syncFloatingResizePosition();
  }

  void View::resizeFloating(int width, int height) {
    syncFloatingResizePosition();
    if (m_fixedRender) {
      resizeFixedRenderBox(width, height);
      return;
    }
    requestFloatingSize(width, height);
  }

  void View::resizeFloatingEdge(uint32_t edges, double delta) {
    // Mirror the guard set of resizeFloatingFractions: a fullscreen view owns its
    // size, a tiled one has no floating box, and a view with no usable area has
    // nothing to size against.
    if (!m_mapped || m_tiled || currentFullscreen() || scheduledFullscreen()) {
      return;
    }
    const wlr_box usable = floatingUsableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }
    const bool widthAxis = (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0;
    const auto current = floatingFraction(widthAxis);
    if (!current) {
      return;
    }
    // The same 0.1 fraction floor the fraction path enforces, so a hint-less
    // client cannot collapse to a single pixel.
    const double target = std::clamp(*current + delta, 0.1, 1.0);
    const SizeHints hints = sizeHints();
    const auto [basisWidth, basisHeight] = floatingSize();
    // A pending size and the position animation target form one logical box.
    // Anchor that box rather than the in-flight scene node, so repeated actions
    // keep the same far edge while the previous resize is still animating.
    const wlr_box& geo = geometryBox();
    const wlr_box anchor{
        .x = layoutTargetX() + geo.x,
        .y = layoutTargetY() + geo.y,
        .width = basisWidth,
        .height = basisHeight,
    };
    const int width = widthAxis
        ? (m_fixedRender ? floatingFractionSize(target, usable.width)
                         : clampWidth(floatingFractionSize(target, usable.width), hints))
        : basisWidth;
    const int height = widthAxis
        ? basisHeight
        : (m_fixedRender ? floatingFractionSize(target, usable.height)
                         : clampHeight(floatingFractionSize(target, usable.height), hints));
    if (width <= 0 || height <= 0) {
      return;
    }
    // Pin the edge opposite the named one, then take the same steps as the fraction path: drop a maximize, animate the
    // change, keep the window on screen. The origin animates with the size (same duration and curve), so the opposite
    // edge stays put while the client catches up.
    //
    // The anchor session stays open until the client answers the configure, so a commit at a different size still
    // recomputes the origin from the actual geometry. finishFloatingResize ends the session but keeps the anchor until
    // the request settles; that commit re-pins the edge and retires the anchor.
    if (m_fixedRender) {
      m_floating.beginResize({layoutX(), layoutY(), m_fixedBoxWidth, m_fixedBoxHeight}, edges);
      resizeFixedRenderBox(width, height);
      finishFloatingResize();
      return;
    }
    const FloatingPoint anchoredOrigin =
        anchoredContentOrigin(anchor, edges, {.x = 0, .y = 0, .width = width, .height = height});
    m_floating.beginResize(anchor, edges);
    clearRegionState();
    dropMaximizedForResize();
    requestFloatingSize(width, height);
    beginResizeAnimation(width, height);
    animateTo(anchoredOrigin.x - geo.x, anchoredOrigin.y - geo.y);
    clampFloatingPositionForSize(width, height);
    finishFloatingResize();
  }

  void View::finishFloatingResize() { m_floating.endResize(); }

  void View::syncFloatingResizePosition() {
    // An attached dialog resizes around its centre, which its parent's layoutAttachments() keeps.
    if (m_fixedRender || !m_floating.anchor() || attached()) {
      return;
    }
    const wlr_box& geo = geometryBox();
    const FloatingPoint content = anchoredContentOrigin(*m_floating.anchor(), m_floating.edges(), geo);
    const int x = content.x - geo.x;
    const int y = content.y - geo.y;
    // A keybind resize animates the origin with the size, so a commit implying a different origin retargets that
    // animation (a matching target is left alone). A pointer drag places the origin directly to follow the cursor.
    if ((m_posX.animating() || m_posY.animating()) && !sizeGrabActive()) {
      if (layoutTargetX() != x || layoutTargetY() != y) {
        animateTo(x, y);
      }
      return;
    }
    setPosition(x, y);
  }

  void View::adoptFloatingClientSize() {
    if (m_tiled || !m_mapped || scheduledFullscreen() || scheduledMaximized() || m_region) {
      return;
    }
    // El piloto de render fijo nunca adopta el tamaño del cliente como caja:
    // un commit del juego cambia su resolución, no la ventana.
    if (m_fixedRender) {
      return;
    }
    if (!retireFloatingSizeRequest()) {
      return;
    }
    const wlr_box geo = geometryBox();
    if (geo.width <= 0 || geo.height <= 0) {
      return;
    }
    if (scheduledSize().width != geo.width || scheduledSize().height != geo.height) {
      // Once the latest compositor size request is committed, a floating
      // client owns its size. Direct assignment avoids an echo configure.
      adoptScheduledSize(geo.width, geo.height);
      const wlr_box usable = floatingUsableArea();
      if (m_workspace != nullptr
          && m_workspace->layoutMode() == LayoutMode::Stacking
          && !m_floating.restoredOrigin(usable)
          && transientParent() == nullptr) {
        placeInUsableArea();
      } else {
        clampFloatingPosition();
      }
    }
  }

  void View::syncFloatingSurfaceClip() {
    if (m_tiled || scheduledFullscreen()) {
      return;
    }
    if (m_fixedRender) {
      // La superficie se escala dentro de la caja; recortarla al tamaño del
      // cliente dejaría solo una esquina visible.
      setSurfaceTreeClip(nullptr);
      updateBlur();
      updateShadow();
      return;
    }
    const wlr_box& geo = geometryBox();
    int width = scheduledSize().width;
    int height = scheduledSize().height;
    if (width <= 0) {
      width = currentSize().width;
    }
    if (height <= 0) {
      height = currentSize().height;
    }
    // Electron often keeps a wide buffer while tiled; without a clip, toggling float
    // would suddenly show the full surface.
    if (width > 0 && height > 0 && (geo.width > width || geo.height > height)) {
      const wlr_box clip{geo.x, geo.y, std::min(geo.width, width), std::min(geo.height, height)};
      setSurfaceTreeClip(&clip);
    } else {
      setSurfaceTreeClip(nullptr);
    }
    updateBlur();
    updateShadow();
  }
} // namespace umbriel
