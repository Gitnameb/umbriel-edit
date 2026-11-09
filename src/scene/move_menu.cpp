#include "scene/move_menu.h"

#include "config/config.h"
#include "output/output.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/actions.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/workspace.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include "wlr.h"
// clang-format on
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace umbriel {

  namespace {
    constexpr int kCardWidth = 46;
    constexpr int kRowHeight = 32;
    constexpr int kPadding = 5;
    constexpr int kRowGap = 2;
    constexpr int kMargin = 8;
    constexpr int kOffsetY = 26;
    constexpr int kRadius = 13;
    constexpr int kRowRadius = 8;
    constexpr int kMaxRows = 9;
  } // namespace

  MoveMenu::MoveMenu(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  MoveMenu::~MoveMenu() { hide(); }

  void MoveMenu::show(View* view, double anchorX, double anchorY) {
    if (view == nullptr) {
      hide();
      return;
    }
    Workspace* workspace = view->workspace();
    WorkspaceGroup* group = workspace != nullptr ? workspace->group() : nullptr;
    // Una ventana del scratchpad no tiene escritorio propio; el selector usa
    // el grupo de su salida, que es donde puede aterrizar.
    if (group == nullptr) {
      Output* output = view->currentOutput();
      group = output != nullptr ? output->workspaceGroup() : nullptr;
    }
    if (group == nullptr) {
      hide();
      return;
    }
    const size_t count = std::min<size_t>(group->workspaceCount(), kMaxRows);
    if (count == 0) {
      hide();
      return;
    }

    hide();
    m_view = view;
    m_group = group;
    m_count = count;
    const size_t current = workspace != nullptr
        ? workspace->index()
        : (group->active() != nullptr ? group->active()->index() : 0);
    m_current = std::min(current, count - 1);
    m_hoverRow = -1;
    m_cardWidth = kCardWidth;
    m_padding = kPadding;
    m_rowHeight = kRowHeight;
    m_rowGap = kRowGap;
    m_cardHeight = kPadding * 2 + kRowHeight * static_cast<int>(count) + kRowGap * (static_cast<int>(count) - 1);

    Output* output = view->currentOutput();
    if (output == nullptr) {
      output = m_server.outputFromWlr(m_server.preferredOutput());
    }
    wlr_box outputBox{};
    if (output != nullptr) {
      wlr_output_layout_get_box(m_server.outputLayout(), output->wlr(), &outputBox);
    }
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      outputBox = {.x = 0, .y = 0, .width = 1920, .height = 1080};
    }

    const double desiredX = anchorX - m_cardWidth / 2.0;
    const double desiredY = anchorY + kOffsetY;
    m_cardX = std::round(std::clamp(
        desiredX, static_cast<double>(outputBox.x + kMargin),
        static_cast<double>(outputBox.x + outputBox.width - m_cardWidth - kMargin)
    ));
    m_cardY = std::round(std::clamp(
        desiredY, static_cast<double>(outputBox.y + kMargin),
        static_cast<double>(outputBox.y + outputBox.height - m_cardHeight - kMargin)
    ));

    render();
  }

  void MoveMenu::hide() {
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
    m_view = nullptr;
    m_group = nullptr;
    m_count = 0;
    m_current = 0;
    m_hoverRow = -1;
    m_currentRow = nullptr;
    m_hoverRowRect = nullptr;
  }

  void MoveMenu::forgetView(const View* view) {
    if (m_view == view) {
      hide();
    }
  }

  bool MoveMenu::contains(double x, double y) const {
    return x >= m_cardX && x < m_cardX + m_cardWidth && y >= m_cardY && y < m_cardY + m_cardHeight;
  }

  int MoveMenu::rowAt(double x, double y) const {
    if (m_tree == nullptr || !contains(x, y)) {
      return -1;
    }
    const double localX = x - m_cardX - m_padding;
    const double localY = y - m_cardY - m_padding;
    if (localX < 0.0 || localX >= m_cardWidth - 2.0 * m_padding || localY < 0.0) {
      return -1;
    }
    const int stride = m_rowHeight + m_rowGap;
    const int row = static_cast<int>(localY) / stride;
    if (row < 0 || static_cast<size_t>(row) >= m_count) {
      return -1;
    }
    if (static_cast<int>(localY) % stride >= m_rowHeight) {
      return -1;
    }
    return row;
  }

  void MoveMenu::updateHover(double x, double y) {
    setHoverRow(rowAt(x, y));
  }

  void MoveMenu::setHoverRow(int row) {
    if (m_hoverRow == row) {
      return;
    }
    m_hoverRow = row;
    if (m_hoverRowRect == nullptr) {
      return;
    }
    // El escritorio actual ya tiene su propio resaltado.
    if (row < 0 || static_cast<size_t>(row) == m_current) {
      wlr_scene_node_set_enabled(&m_hoverRowRect->node, false);
      return;
    }
    wlr_scene_node_set_position(
        &m_hoverRowRect->node, m_padding, m_padding + row * (m_rowHeight + m_rowGap)
    );
    wlr_scene_node_set_enabled(&m_hoverRowRect->node, true);
  }

  bool MoveMenu::handlePress(double x, double y, uint32_t button) {
    if (m_tree == nullptr) {
      return false;
    }
    if (button != BTN_LEFT) {
      hide();
      return true;
    }
    const int row = rowAt(x, y);
    if (row < 0) {
      hide();
      return true;
    }
    choose(static_cast<size_t>(row));
    return true;
  }

  bool MoveMenu::handleKey(uint32_t keysym) {
    if (m_tree == nullptr) {
      return false;
    }
    if (keysym == XKB_KEY_Escape) {
      hide();
      return true;
    }
    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
      choose(static_cast<size_t>(keysym - XKB_KEY_1));
      return true;
    }
    if (keysym >= XKB_KEY_KP_1 && keysym <= XKB_KEY_KP_9) {
      choose(static_cast<size_t>(keysym - XKB_KEY_KP_1));
      return true;
    }
    // El menú es modal mientras está abierto: una tecla suelta no debe
    // escribir en la ventana que quedó debajo.
    return true;
  }

  void MoveMenu::choose(size_t index) {
    View* view = m_view;
    WorkspaceGroup* group = m_group;
    const size_t current = m_current;
    hide();
    if (view == nullptr || group == nullptr || index >= kMaxRows || index == current) {
      return;
    }
    // Como el selector del shell: la ventana se va al destino y el asiento se
    // queda donde estaba.
    (void)moveViewToWorkspaceAt(m_server, *view, *group, index, false);
  }

  void MoveMenu::render() {
    if (m_view == nullptr || m_count == 0) {
      return;
    }
    Output* output = m_view->currentOutput();
    if (output == nullptr) {
      output = m_server.outputFromWlr(m_server.preferredOutput());
    }
    const double scale = output != nullptr ? std::max(1.0, std::ceil(static_cast<double>(output->wlr()->scale))) : 1.0;

    wlr_scene_node_raise_to_top(&m_parent->node);
    m_tree = wlr_scene_tree_create(m_parent);
    if (m_tree == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&m_tree->node, static_cast<int>(m_cardX), static_cast<int>(m_cardY));
    m_shadow.update(m_tree, m_cardWidth, m_cardHeight, 1, kRadius);

    const auto& colors = config().colors;
    float borderColor[4]{};
    float backgroundColor[4]{};
    float currentColor[4]{};
    float hoverColor[4]{};
    premultiplied(borderColor, colors.border.focused, 0.45F);
    premultiplied(backgroundColor, colors.background, 0.97F);
    premultiplied(currentColor, colors.accentPrimary, 0.16F);
    premultiplied(hoverColor, colors.textPrimary, 0.07F);

    wlr_scene_rect* border = wlr_scene_rect_create(m_tree, m_cardWidth, m_cardHeight, borderColor);
    if (border != nullptr) {
      wlr_scene_rect_set_corner_radius(border, kRadius);
      border->accepts_input = false;
    }
    wlr_scene_rect* panel =
        wlr_scene_rect_create(m_tree, m_cardWidth - 2, m_cardHeight - 2, backgroundColor);
    if (panel != nullptr) {
      wlr_scene_node_set_position(&panel->node, 1, 1);
      wlr_scene_rect_set_corner_radius(panel, kRadius - 1);
      panel->accepts_input = false;
    }

    const int rowWidth = m_cardWidth - 2 * m_padding;
    m_currentRow = wlr_scene_rect_create(m_tree, rowWidth, m_rowHeight, currentColor);
    if (m_currentRow != nullptr) {
      wlr_scene_rect_set_corner_radius(m_currentRow, kRowRadius);
      m_currentRow->accepts_input = false;
      wlr_scene_node_set_position(
          &m_currentRow->node, m_padding, m_padding + static_cast<int>(m_current) * (m_rowHeight + m_rowGap)
      );
    }
    m_hoverRowRect = wlr_scene_rect_create(m_tree, rowWidth, m_rowHeight, hoverColor);
    if (m_hoverRowRect != nullptr) {
      wlr_scene_rect_set_corner_radius(m_hoverRowRect, kRowRadius);
      m_hoverRowRect->accepts_input = false;
      wlr_scene_node_set_enabled(&m_hoverRowRect->node, false);
    }

    // Un buffer por número: Pango devuelve el ancho natural y así el dígito
    // queda centrado en la celda sin depender de una fuente monoespaciada.
    for (size_t index = 0; index < m_count; ++index) {
      const bool current = index == m_current;
      const std::string markup = std::format(
          "<span foreground='{}' weight='bold'>{}</span>",
          rgbaHex(current ? colors.accentPrimary : colors.textPrimary), index + 1
      );
      TextBufferResult rendered = renderTextBuffer({
          .markup = markup,
          .font = "sans 11",
          .maxWidth = rowWidth,
          .padding = 0,
          .scale = scale,
          .bgR = 0.0,
          .bgG = 0.0,
          .bgB = 0.0,
          .bgA = 0.0,
      });
      if (rendered.buffer == nullptr) {
        continue;
      }
      wlr_scene_buffer* node = wlr_scene_buffer_create(m_tree, rendered.buffer);
      wlr_buffer_drop(rendered.buffer);
      if (node == nullptr) {
        continue;
      }
      node->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
      wlr_scene_buffer_set_dest_size(node, rendered.logicalWidth, rendered.logicalHeight);
      const int x = m_padding + (rowWidth - rendered.logicalWidth) / 2;
      const int y = m_padding + static_cast<int>(index) * (m_rowHeight + m_rowGap)
          + (m_rowHeight - rendered.logicalHeight) / 2;
      wlr_scene_node_set_position(&node->node, x, y);
    }
  }

} // namespace umbriel
