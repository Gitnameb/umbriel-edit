#pragma once

#include "scene/ssd_frame_geometry.h"
#include "scene/surface_blur.h"
#include "view/chrome_attachment.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wlr_scene_node;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_blur;
struct wlr_buffer;

namespace umbriel {
  class TabLabelCache;

  class SsdFrame final : public ViewChromeAttachment {
  public:
    SsdFrame(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent = nullptr);
    ~SsdFrame() override;
    void layout(const ViewChromeGeometry& geometry) override;
    void setFocused(bool focused) override;
    void setAlpha(float alpha) override;
    // Like surface blur, backdrop blur follows lifecycle fades independently of the chrome's drag opacity.
    void setAlphas(float chromeAlpha, float blurAlpha);
    void reloadConfig() override;
    void setTitle(std::string_view title);
    void setSubtitle(std::string_view subtitle);
    void setAppId(std::string_view appId);
    void setResizable(bool resizable);
    void setMaximizeEnabled(bool enabled);
    // The buttons to show, from the title outward.
    void setButtons(std::span<const TitleBarButton> buttons);
    void setMaximized(bool maximized);
    void setFullscreen(bool fullscreen);
    void setPinned(bool pinned);
    void setFloating(bool floating);
    // Overlay chrome is hidden until the compositor shows it with Mod held.
    void setOverlayVisible(bool visible);
    // Brings overlay chrome above the client content it covers.
    void raise();
    void setHovered(SsdFramePart part);
    void setPressed(SsdFramePart part);
    void setBlurOptions(const SurfaceBlurOptions& options);
    void setBlurLive(bool live);
    bool maximizeEnabled() const { return m_maximizeEnabled; }
    int titleBarHeight() const { return m_geometry.titleBarHeight; }
    // Whether the bar is drawn inside the content and shown only with Mod.
    bool titleBarOverlayMode() const { return m_geometry.titleBarOverlay; }
    bool contains(const wlr_scene_node* node) const;
    SsdFramePoint pointAt(double x, double y) const;
    void snapshot(wlr_scene_tree* parent) const;
    std::unique_ptr<ViewChromeAttachment> makePreview(wlr_scene_tree* parent) const override;
    void syncPreview(ViewChromeAttachment& preview) const override;

  private:
    void redraw();
    void updateColors();
    void updateButtonIcon(TitleBarButton button, const std::array<float, 4>& color, float alpha, bool visible);
    std::span<const TitleBarButton> visibleButtons() const;
    void updateBlur();
    int zoomed(int value) const;
    wlr_scene_tree* m_tree = nullptr;
    // Invisible resize bands stay outside the window's animation capture bounds.
    wlr_scene_tree* m_gripTree = nullptr;
    // Invisible border behind each floating pill, then the pill fill.
    wlr_scene_rect* m_headBorder = nullptr;
    wlr_scene_rect* m_background = nullptr;
    wlr_scene_rect* m_controlsBorder = nullptr;
    wlr_scene_rect* m_controlsBackground = nullptr;
    wlr_scene_blur* m_blur = nullptr;
    wlr_scene_blur* m_controlsBlur = nullptr;
    SurfaceBlurOptions m_blurOptions;
    bool m_blurLive = false;
    wlr_scene_buffer* m_label = nullptr;
    wlr_scene_buffer* m_subtitleLabel = nullptr;
    wlr_scene_buffer* m_appIcon = nullptr;
    wlr_scene_rect* m_close = nullptr;
    wlr_scene_rect* m_maximize = nullptr;
    wlr_scene_rect* m_minimize = nullptr;
    wlr_scene_rect* m_pin = nullptr;
    wlr_scene_rect* m_float = nullptr;
    wlr_scene_rect* m_move = nullptr;
    wlr_scene_rect* m_fullscreenButton = nullptr;
    wlr_scene_rect* m_volume = nullptr;
    std::array<wlr_scene_buffer*, kTitleBarButtonCount> m_icons{};
    // Keep a consumer lock for snapshots and renderer recovery after the scene uploads an icon's texture.
    std::array<wlr_buffer*, kTitleBarButtonCount> m_iconBuffers{};
    std::array<std::array<float, 4>, kTitleBarButtonCount> m_iconColors{};
    std::array<int, kTitleBarButtonCount> m_iconSizes{};
    std::vector<TitleBarButton> m_buttons;
    std::array<wlr_scene_rect*, 4> m_grips{};
    std::shared_ptr<TabLabelCache> m_labels;
    ViewChromeGeometry m_geometry;
    std::string m_title;
    std::string m_subtitle;
    std::string m_appId;
    std::string m_iconKey;
    bool m_focused = false;
    bool m_resizable = false;
    bool m_maximizeEnabled = true;
    bool m_maximized = false;
    bool m_fullscreen = false;
    bool m_pinned = false;
    bool m_floating = false;
    bool m_preview = false;
    bool m_overlayVisible = true;
    float m_alpha = 1.0F;
    float m_blurAlpha = 1.0F;
    SsdFramePart m_hovered = SsdFramePart::None;
    SsdFramePart m_pressed = SsdFramePart::None;
  };
} // namespace umbriel
