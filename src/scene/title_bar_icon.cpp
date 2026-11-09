#include "scene/title_bar_icon.h"

#include "scene/cairo_buffer.h"

#include <algorithm>
#include <cmath>
#include <cairo.h>

namespace umbriel {
  namespace {
    // Los SVG del mockup usan un lienzo de 24×24 con trazo de 1,8 y esquinas
    // redondeadas. Todo se dibuja en esas unidades y luego se escala al botón.
    constexpr double kCanvas = 24.0;
    constexpr double kStroke = 1.8;

    void roundedRect(cairo_t* cr, double x, double y, double width, double height, double radius) {
      const double r = std::min(radius, std::min(width, height) / 2.0);
      cairo_new_sub_path(cr);
      cairo_arc(cr, x + width - r, y + r, r, -M_PI / 2.0, 0.0);
      cairo_arc(cr, x + width - r, y + height - r, r, 0.0, M_PI / 2.0);
      cairo_arc(cr, x + r, y + height - r, r, M_PI / 2.0, M_PI);
      cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
      cairo_close_path(cr);
    }
  } // namespace

  wlr_buffer* renderTitleBarIcon(TitleBarButton button, int pixels, const std::array<float, 4>& color) {
    pixels = std::max(1, pixels);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    cairo_t* cr = cairo_create(surface);

    // En el mockup el símbolo mide 13 px dentro de un botón de 26 px: la mitad.
    // La almohadilla de «mover» es un glifo de texto algo mayor, como el «#».
    const double glyph = pixels * (button == TitleBarButton::Move ? 0.58 : 0.5);
    const double scale = glyph / kCanvas;
    cairo_translate(cr, (pixels - glyph) / 2.0, (pixels - glyph) / 2.0);
    cairo_scale(cr, scale, scale);

    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, kStroke);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (button) {
    case TitleBarButton::Close:
      // i-close: m6 6 12 12 M18 6 6 18
      cairo_move_to(cr, 6, 6);
      cairo_line_to(cr, 18, 18);
      cairo_move_to(cr, 18, 6);
      cairo_line_to(cr, 6, 18);
      break;
    case TitleBarButton::Maximize:
      // Marco cuadrado y más grande que el antiguo, con cuatro esquinas
      // interiores como la referencia de Descargas. Sin línea de barra de
      // título.
      roundedRect(cr, 3, 3, 18, 18, 3);
      cairo_stroke(cr);
      cairo_new_path(cr);
      cairo_set_line_width(cr, 2.0);
      cairo_move_to(cr, 7.5, 11);
      cairo_line_to(cr, 7.5, 7.5);
      cairo_line_to(cr, 11, 7.5);
      cairo_move_to(cr, 16.5, 11);
      cairo_line_to(cr, 16.5, 7.5);
      cairo_line_to(cr, 13, 7.5);
      cairo_move_to(cr, 7.5, 13);
      cairo_line_to(cr, 7.5, 16.5);
      cairo_line_to(cr, 11, 16.5);
      cairo_move_to(cr, 16.5, 13);
      cairo_line_to(cr, 16.5, 16.5);
      cairo_line_to(cr, 13, 16.5);
      break;
    case TitleBarButton::Minimize:
      // i-minus: M5 12h14
      cairo_move_to(cr, 5, 12);
      cairo_line_to(cr, 19, 12);
      break;
    case TitleBarButton::Float:
      // Dos ventanas superpuestas. La de atrás se recorta con el contorno de
      // la de delante para que no quede una línea cruzada que parezca una
      // barra de título. Sin línea interior.
      roundedRect(cr, 9, 3, 12, 12, 2);
      cairo_stroke(cr);
      cairo_new_path(cr);
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      roundedRect(cr, 3, 9, 12, 12, 2);
      cairo_fill(cr);
      cairo_restore(cr);
      cairo_new_path(cr);
      roundedRect(cr, 3, 9, 12, 12, 2);
      break;
    case TitleBarButton::Pin:
      // i-pin: M9 3h6l-1 5 3 3v2H7v-2l3-3-1-5z M12 13v8
      cairo_move_to(cr, 9, 3);
      cairo_line_to(cr, 15, 3);
      cairo_line_to(cr, 14, 8);
      cairo_line_to(cr, 17, 11);
      cairo_line_to(cr, 17, 13);
      cairo_line_to(cr, 7, 13);
      cairo_line_to(cr, 7, 11);
      cairo_line_to(cr, 10, 8);
      cairo_line_to(cr, 9, 3);
      cairo_close_path(cr);
      cairo_move_to(cr, 12, 13);
      cairo_line_to(cr, 12, 21);
      break;
    case TitleBarButton::Move:
      // «#» como en el mockup: el botón abre el selector de escritorio.
      cairo_set_line_width(cr, 2.1);
      cairo_move_to(cr, 10, 4);
      cairo_line_to(cr, 8.5, 20);
      cairo_move_to(cr, 16, 4);
      cairo_line_to(cr, 14.5, 20);
      cairo_move_to(cr, 4.5, 9);
      cairo_line_to(cr, 20, 9);
      cairo_move_to(cr, 4, 15);
      cairo_line_to(cr, 19.5, 15);
      break;
    case TitleBarButton::Fullscreen:
      // Cuatro esquinas, como la referencia de Descargas.
      cairo_move_to(cr, 4, 9);
      cairo_line_to(cr, 4, 4);
      cairo_line_to(cr, 9, 4);
      cairo_move_to(cr, 15, 4);
      cairo_line_to(cr, 20, 4);
      cairo_line_to(cr, 20, 9);
      cairo_move_to(cr, 20, 15);
      cairo_line_to(cr, 20, 20);
      cairo_line_to(cr, 15, 20);
      cairo_move_to(cr, 9, 20);
      cairo_line_to(cr, 4, 20);
      cairo_line_to(cr, 4, 15);
      break;
    case TitleBarButton::Volume:
      // Altavoz y dos ondas, como el botón de volumen del mockup.
      cairo_move_to(cr, 3.5, 10);
      cairo_line_to(cr, 6.5, 10);
      cairo_line_to(cr, 10.5, 6.5);
      cairo_line_to(cr, 10.5, 17.5);
      cairo_line_to(cr, 6.5, 14);
      cairo_line_to(cr, 3.5, 14);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_new_path(cr);
      cairo_set_line_width(cr, 1.7);
      cairo_arc(cr, 11.5, 12, 3.2, -0.85, 0.85);
      cairo_stroke(cr);
      cairo_new_path(cr);
      cairo_arc(cr, 11.5, 12, 6.2, -0.75, 0.75);
      cairo_stroke(cr);
      cairo_new_path(cr);
      break;
    }

    cairo_stroke(cr);
    cairo_destroy(cr);
    return createCairoBuffer(surface);
  }
} // namespace umbriel
