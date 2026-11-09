#pragma once

#include "scene/surface_shadow.h"

#include <cstddef>
#include <cstdint>

struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  class Server;
  class View;
  class WorkspaceGroup;

  // Selector 1–9 del botón «Mover» de la barra de título. Vive en Umbriel,
  // como el resto de la decoración: una tarjeta estrecha junto al puntero con
  // el escritorio actual resaltado. El teclado y el ratón los atienden
  // Keyboard y Cursor; esta clase solo dibuja y resuelve el destino.
  class MoveMenu {
  public:
    MoveMenu(Server& server, wlr_scene_tree* parent);
    ~MoveMenu();

    MoveMenu(const MoveMenu&) = delete;
    MoveMenu& operator=(const MoveMenu&) = delete;

    void show(View* view, double anchorX, double anchorY);
    void hide();
    bool visible() const { return m_tree != nullptr; }
    View* view() const { return m_view; }
    // Una ventana destruida no puede dejar el menú apuntando a memoria libre.
    void forgetView(const View* view);

    bool contains(double x, double y) const;
    int rowAt(double x, double y) const;
    void updateHover(double x, double y);
    // Cualquier pulsación se consume mientras el menú está abierto: dentro
    // elige un escritorio y fuera solo lo cierra.
    bool handlePress(double x, double y, uint32_t button);
    // Escape cierra; 1–9 eligen; el resto no llega a la ventana de debajo.
    bool handleKey(uint32_t keysym);

  private:
    void render();
    void choose(size_t index);
    void setHoverRow(int row);

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    SurfaceShadow m_shadow;

    View* m_view = nullptr;
    WorkspaceGroup* m_group = nullptr;
    size_t m_count = 0;
    size_t m_current = 0;
    int m_hoverRow = -1;

    // Coordenadas globales de la tarjeta.
    double m_cardX = 0.0;
    double m_cardY = 0.0;
    int m_cardWidth = 46;
    int m_cardHeight = 0;
    int m_padding = 5;
    int m_rowHeight = 32;
    int m_rowGap = 2;

    wlr_scene_rect* m_currentRow = nullptr;
    wlr_scene_rect* m_hoverRowRect = nullptr;
  };

} // namespace umbriel
