#include "scene/app_icon.h"

#include "scene/cairo_buffer.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace umbriel {
  namespace {
    namespace fs = std::filesystem;

    std::string lower(std::string_view value) {
      std::string out(value);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }

    std::string homeDir() {
      const char* home = std::getenv("HOME");
      return home != nullptr ? std::string(home) : std::string();
    }

    std::vector<fs::path> dataDirs() {
      std::vector<fs::path> dirs;
      const std::string home = homeDir();
      if (!home.empty()) {
        dirs.emplace_back(fs::path(home) / ".local/share");
        dirs.emplace_back(fs::path(home) / ".local/share/flatpak/exports/share");
      }
      dirs.emplace_back("/usr/local/share");
      dirs.emplace_back("/usr/share");
      dirs.emplace_back("/var/lib/flatpak/exports/share");
      return dirs;
    }

    // Lee una clave sencilla («Icon=», «StartupWMClass=») de un .desktop.
    std::string desktopValue(const fs::path& file, std::string_view key) {
      std::ifstream stream(file);
      if (!stream)
        return {};
      std::string line;
      const std::string prefix = std::string(key) + "=";
      while (std::getline(stream, line)) {
        if (line.rfind(prefix, 0) == 0) {
          std::string value = line.substr(prefix.size());
          while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
          const size_t start = value.find_first_not_of(' ');
          return start == std::string::npos ? std::string() : value.substr(start);
        }
      }
      return {};
    }

    // Nombre de icono declarado por la app. Se prueba el .desktop con el mismo
    // id y, si no existe, se busca por StartupWMClass o por nombre de archivo.
    std::string desktopIcon(const std::string& appId) {
      static std::unordered_map<std::string, std::string> cache;
      if (const auto found = cache.find(appId); found != cache.end())
        return found->second;

      const std::string wanted = lower(appId);
      std::string icon;

      for (const fs::path& base : dataDirs()) {
        const fs::path applications = base / "applications";
        std::error_code error;

        const fs::path direct = applications / (appId + ".desktop");
        if (fs::is_regular_file(direct, error)) {
          icon = desktopValue(direct, "Icon");
          if (!icon.empty())
            break;
        }

        if (!fs::is_directory(applications, error))
          continue;
        for (const fs::directory_entry& entry : fs::directory_iterator(applications, error)) {
          if (error || !entry.is_regular_file() || entry.path().extension() != ".desktop")
            continue;
          if (lower(entry.path().stem().string()) == wanted) {
            icon = desktopValue(entry.path(), "Icon");
            if (!icon.empty())
              break;
          }
          const std::string wmClass = lower(desktopValue(entry.path(), "StartupWMClass"));
          if (!wmClass.empty() && wmClass == wanted) {
            icon = desktopValue(entry.path(), "Icon");
            if (!icon.empty())
              break;
          }
        }
        if (!icon.empty())
          break;
      }

      if (icon.empty())
        icon = appId; // muchas apps usan su app_id como nombre de icono
      cache.emplace(appId, icon);
      return icon;
    }

    bool isAbsoluteIcon(const std::string& icon, fs::path* out) {
      if (icon.empty() || icon.front() != '/')
        return false;
      std::error_code error;
      if (!fs::is_regular_file(icon, error))
        return false;
      *out = icon;
      return true;
    }

    // Busca «Icon=» en los temas instalados. Primero breeze-dark (el tema que
    // usa Shell), después los demás; los iconos de apps de terceros suelen estar
    // en hicolor.
    std::string findIconFile(const std::string& icon) {
      static constexpr std::array<std::string_view, 8> themes{
          "breeze-dark", "breeze", "Breeze_Light", "hicolor", "Adwaita", "oxygen", "locolor", "default"};
      // Breeze usa carpetas numéricas («48»), hicolor usa «48x48».
      static constexpr std::array<std::string_view, 17> sizes{
          "scalable", "256x256", "256", "128x128", "128", "64x64", "64", "48x48", "48",
          "32x32", "32", "24x24", "24", "22x22", "22", "16x16", "16"};
      static constexpr std::array<std::string_view, 3> categories{"apps", "applications", "mimetypes"};
      static constexpr std::array<std::string_view, 3> extensions{".svg", ".png", ".xpm"};

      std::vector<fs::path> roots;
      const std::string home = homeDir();
      if (!home.empty()) {
        roots.emplace_back(fs::path(home) / ".local/share/icons");
        roots.emplace_back(fs::path(home) / ".icons");
        roots.emplace_back(fs::path(home) / ".local/share/flatpak/exports/share/icons");
      }
      roots.emplace_back("/usr/local/share/icons");
      roots.emplace_back("/usr/share/icons");
      roots.emplace_back("/var/lib/flatpak/exports/share/icons");
      roots.emplace_back("/usr/share/pixmaps");

      std::error_code error;
      for (const fs::path& root : roots) {
        // /usr/share/pixmaps guarda los archivos directamente.
        if (root.filename() == "pixmaps") {
          for (const std::string_view extension : extensions) {
            const fs::path candidate = root / (icon + std::string(extension));
            if (fs::is_regular_file(candidate, error))
              return candidate.string();
          }
          continue;
        }
        if (!fs::is_directory(root, error))
          continue;
        for (const std::string_view theme : themes) {
          const fs::path themeRoot = root / theme;
          if (!fs::is_directory(themeRoot, error))
            continue;
          for (const std::string_view size : sizes) {
            for (const std::string_view category : categories) {
              // hicolor: <tema>/48x48/apps. Breeze: <tema>/apps/48.
              const std::array<fs::path, 2> folders{
                  themeRoot / size / category, themeRoot / category / size};
              for (const fs::path& folder : folders) {
                if (!fs::is_directory(folder, error))
                  continue;
                for (const std::string_view extension : extensions) {
                  const fs::path candidate = folder / (icon + std::string(extension));
                  if (fs::is_regular_file(candidate, error))
                    return candidate.string();
                }
              }
            }
          }
        }
      }
      return {};
    }

    std::string resolveIconPath(const std::string& appId) {
      static std::unordered_map<std::string, std::string> cache;
      if (const auto found = cache.find(appId); found != cache.end())
        return found->second;

      fs::path absolute;
      const std::string icon = desktopIcon(appId);
      std::string resolved;
      if (isAbsoluteIcon(icon, &absolute))
        resolved = absolute.string();
      else
        resolved = findIconFile(icon);
      cache.emplace(appId, resolved);
      return resolved;
    }

    cairo_surface_t* pixbufToSurface(GdkPixbuf* pixbuf) {
      const int width = gdk_pixbuf_get_width(pixbuf);
      const int height = gdk_pixbuf_get_height(pixbuf);
      const int channels = gdk_pixbuf_get_n_channels(pixbuf);
      const int stride = gdk_pixbuf_get_rowstride(pixbuf);
      const bool alpha = gdk_pixbuf_get_has_alpha(pixbuf) != 0;
      const guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
      if (width <= 0 || height <= 0 || pixels == nullptr)
        return nullptr;

      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
      cairo_surface_flush(surface);
      auto* destination = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(surface));
      const int destinationStride = cairo_image_surface_get_stride(surface) / 4;
      for (int y = 0; y < height; ++y) {
        const guchar* row = pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
          const guchar* source = row + static_cast<size_t>(x) * channels;
          const uint32_t a = alpha ? source[3] : 255U;
          const uint32_t r = static_cast<uint32_t>(source[0]) * a / 255U;
          const uint32_t g = static_cast<uint32_t>(source[1]) * a / 255U;
          const uint32_t b = static_cast<uint32_t>(source[2]) * a / 255U;
          destination[static_cast<size_t>(y) * destinationStride + x] = (a << 24U) | (r << 16U) | (g << 8U) | b;
        }
      }
      cairo_surface_mark_dirty(surface);
      return surface;
    }
  } // namespace

  wlr_buffer* renderAppIcon(const std::string& appId, int pixels) {
    if (appId.empty() || pixels < 8)
      return nullptr;
    const std::string path = resolveIconPath(appId);
    if (path.empty())
      return nullptr;

    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(path.c_str(), pixels, pixels, TRUE, &error);
    if (pixbuf == nullptr) {
      if (error != nullptr)
        g_error_free(error);
      return nullptr;
    }
    cairo_surface_t* icon = pixbufToSurface(pixbuf);
    g_object_unref(pixbuf);
    if (icon == nullptr)
      return nullptr;

    const int width = cairo_image_surface_get_width(icon);
    const int height = cairo_image_surface_get_height(icon);
    cairo_surface_t* canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
    cairo_t* context = cairo_create(canvas);
    cairo_set_source_surface(
        context, icon, (pixels - width) / 2.0, (pixels - height) / 2.0
    );
    cairo_paint(context);
    cairo_destroy(context);
    cairo_surface_destroy(icon);
    return createCairoBuffer(canvas);
  }
} // namespace umbriel
