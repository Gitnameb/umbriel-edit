#pragma once

#include <string>

struct wlr_buffer;

namespace umbriel {
  // Icono real de la aplicación para la barra de título.
  //
  // Umbriel no depende de GTK ni de un lector de temas de iconos, así que se
  // resuelve el .desktop de la app, se busca su Icon= en los temas instalados y
  // se rasteriza con gdk-pixbuf en un buffer cuadrado de `pixels` píxeles.
  // Devuelve nullptr si no hay icono; el llamador libera el buffer.
  [[nodiscard]] wlr_buffer* renderAppIcon(const std::string& appId, int pixels);
} // namespace umbriel
