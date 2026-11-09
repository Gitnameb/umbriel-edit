#pragma once

extern "C" {
#include <wlr/util/box.h>
}

#include <array>

struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  // Indicadores nativos del modo ventanas. Viven dentro del árbol de la
  // ventana, así que se mueven y redimensionan con ella en el mismo frame, sin
  // el retraso de una capa externa:
  //
  //  · cuatro puntos en las esquinas de la ventana flotante enfocada;
  //  · una línea en cada borde compartido entre mosaicos.
  //
  // Solo se dibujan mientras se mantiene Mod (el mismo estado que muestra las
  // barras de título superpuestas) y nunca sobre la vista general ni el bloqueo.
  class NativeHandles {
  public:
    explicit NativeHandles(wlr_scene_tree* parent);
    ~NativeHandles();
    NativeHandles(const NativeHandles&) = delete;
    NativeHandles& operator=(const NativeHandles&) = delete;

    void setActive(bool active);
    void setFocused(bool focused);
    // Tramos normalizados (0..1) del borde propio que comparte con un vecino.
    // `right` dibuja el divisor vertical y `bottom` el horizontal.
    void setTileEdges(
        bool right, double rightStart, double rightEnd, bool bottom, double bottomStart, double bottomEnd
    );
    // Resalta el divisor que está bajo el puntero. Son dos ejes porque una
    // ventana puede compartir borde derecho e inferior a la vez.
    void setDividerHover(bool right, bool bottom);
    // `gap` es el hueco total entre cajas de contenido (incluye los bordes).
    // `dotsAllowed` deja fuera las flotantes maximizadas y las de pantalla
    // completa; `suppressed` oculta todo (vista general, bloqueo, minimizada).
    void layout(int contentWidth, int contentHeight, int gap, bool tiled, bool dotsAllowed, bool suppressed);

  private:
    void ensureScene();
    void apply();
    void placeDivider(wlr_scene_rect* band, wlr_scene_rect* core, bool vertical, int position, int first, int last);
    void setDividerVisible();

    wlr_scene_tree* m_parent = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    std::array<wlr_scene_rect*, 4> m_dots{};
    wlr_scene_rect* m_rightBand = nullptr;
    wlr_scene_rect* m_rightCore = nullptr;
    wlr_scene_rect* m_bottomBand = nullptr;
    wlr_scene_rect* m_bottomCore = nullptr;

    bool m_active = false;
    bool m_focused = false;
    bool m_tiled = false;
    bool m_dotsAllowed = false;
    bool m_suppressed = false;
    bool m_visible = false;
    bool m_right = false;
    bool m_bottom = false;
    bool m_hoverRight = false;
    bool m_hoverBottom = false;
    double m_rightStart = 0.0;
    double m_rightEnd = 0.0;
    double m_bottomStart = 0.0;
    double m_bottomEnd = 0.0;
    int m_width = 0;
    int m_height = 0;
    int m_gap = 0;
  };

} // namespace umbriel
