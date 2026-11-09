#include "config/config.h"
#include "input/cursor.h"
#include "input/input_capture.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "input/text_input.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "server/actions.h"
#include "server/ipc.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace umbriel {
  namespace {
    uint32_t modifierMaskForKeysym(uint32_t keysym) {
      switch (keysym) {
      case XKB_KEY_Shift_L:
      case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
      case XKB_KEY_Control_L:
      case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
      case XKB_KEY_Alt_L:
      case XKB_KEY_Alt_R:
        return WLR_MODIFIER_ALT;
      case XKB_KEY_Super_L:
      case XKB_KEY_Super_R:
        return WLR_MODIFIER_LOGO;
      default:
        return 0;
      }
    }

  } // namespace

  bool Server::handleVtSwitch(uint32_t keysym, uint32_t modifiers) {
    if (m_session == nullptr) {
      return false;
    }

    unsigned vt = 0;
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
      vt = 1 + (keysym - XKB_KEY_XF86Switch_VT_1);
    } else if (
        (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)
        && keysym >= XKB_KEY_F1
        && keysym <= XKB_KEY_F12
    ) {
      vt = 1 + (keysym - XKB_KEY_F1);
    }

    if (vt == 0) {
      return false;
    }

    if (m_inputCapture != nullptr) {
      m_inputCapture->suspend();
    }
    if (!wlr_session_change_vt(m_session, vt)) {
      wlr_log(WLR_ERROR, "failed to switch to VT %u", vt);
    }
    return true;
  }

  bool Server::cooldownAllows(const Keybind& bind) {
    if (bind.cooldownMs <= 0) {
      return true;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    const auto it = std::ranges::find_if(m_bindCooldowns, [&bind](const BindCooldown& cooldown) {
      return cooldown.submap == bind.submap
          && cooldown.modifiers == bind.modifiers
          && cooldown.useMod == bind.useMod
          && cooldown.modifierOnly == bind.modifierOnly
          && cooldown.keysym == bind.keysym
          && cooldown.wheel == bind.wheel
          && cooldown.mouseButton == bind.mouseButton;
    });
    if (it != m_bindCooldowns.end()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->lastTriggered).count();
      if (elapsed < bind.cooldownMs) {
        return false;
      }
      it->lastTriggered = now;
      return true;
    }
    m_bindCooldowns.push_back({
        .submap = bind.submap,
        .modifiers = bind.modifiers,
        .useMod = bind.useMod,
        .modifierOnly = bind.modifierOnly,
        .keysym = bind.keysym,
        .wheel = bind.wheel,
        .mouseButton = bind.mouseButton,
        .lastTriggered = now,
    });
    return true;
  }

  // An action's reply and the inspection after it see alone rules for its final layout, before the arrange that a
  // sleeping output may not run for a while. Running once per action keeps intermediate layouts out of the rules.
  void Server::refreshPendingAloneRules() {
    for (const auto& output : m_outputs) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (Workspace* workspace = group->workspaceAt(index); workspace != nullptr && workspace->arrangePending()) {
          workspace->refreshAloneRuleStates();
        }
      }
    }
  }

  bool Server::executeAction(const Keybind& bind, std::string* error, bool* cooldownBlocked) {
    if (error != nullptr) {
      error->clear();
    }
    if (cooldownBlocked != nullptr) {
      *cooldownBlocked = false;
    }
    if (bind.action == KeybindAction::None) {
      return false;
    }
    if (bind.cooldownMs > 0 && !cooldownAllows(bind)) {
      if (cooldownBlocked != nullptr) {
        *cooldownBlocked = true;
      }
      return false;
    }
    const ActionHandlerFn handler = actionHandlerFor(bind.action);
    if (handler == nullptr) {
      if (error != nullptr) {
        *error = "action has no handler";
      }
      return false;
    }
    // Config reload may replace the keybind storage while its action runs, so
    // retain the transition before dispatching the primary action.
    const std::optional<SubmapArg> submapAfter = bind.submapAfter;
    Overview* overview = this->overview();
    bool handled = overview != nullptr && overview->handleKeybindAction(bind.action);
    if (!handled) {
      handled = handler(*this, bind, error);
    }
    refreshPendingAloneRules();
    if (!submapAfter.has_value()) {
      return handled;
    }
    if (isSubmapReset(*submapAfter)) {
      if (inSubmap()) {
        popSubmap();
      }
    } else {
      pushSubmap(submapAfter->name);
    }
    return true;
  }

  void
  Server::armModifierTap(const void* source, uint32_t keycode, std::span<const uint32_t> keysyms, uint32_t modifiers) {
    if (m_modifierTap.cancelForKeyPress()) {
      return;
    }

    uint32_t pressedModifier = 0;
    for (const uint32_t keysym : keysyms) {
      pressedModifier |= modifierMaskForKeysym(keysym);
    }
    if (pressedModifier == 0) {
      return;
    }

    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();
    for (const Keybind& bind : config().keybinds) {
      if (!bind.modifierOnly) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (keyboardShortcutsInhibited() && !bind.allowWhenInhibited) {
        continue;
      }
      if (bind.submap != currentSubmap) {
        if (m_activeSubmaps.empty() || !bind.submap.empty() || !isSubmapResetBind(bind)) {
          continue;
        }
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (modifierTapPressMatches(modifiers, pressedModifier, expected)) {
        m_modifierTap.arm(bind, source, keycode);
        return;
      }
    }
  }

  std::optional<Keybind> Server::releaseModifierTap(const void* source, uint32_t keycode) {
    std::optional<Keybind> bind = m_modifierTap.release(source, keycode);
    if (bind && m_sessionLocked && !bind->allowWhenLocked) {
      return std::nullopt;
    }
    if (bind && keyboardShortcutsInhibited() && !bind->allowWhenInhibited) {
      return std::nullopt;
    }
    return bind;
  }

  bool Server::keyboardShortcutsInhibited() const {
    if (m_seat == nullptr) {
      return false;
    }
    wlr_seat* seat = m_seat->wlr();
    wlr_surface* focused = seat->keyboard_state.focused_surface;
    if (!m_sessionLocked && m_manualShortcutsInhibit.enabled(focused)) {
      return true;
    }
    return focused != nullptr
        && clientShortcutsInhibitAllowed(focused)
        && std::ranges::any_of(m_shortcutsInhibitors, [seat, focused](const auto& watch) {
             const wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = watch->inhibitor;
             return inhibitor->active && inhibitor->seat == seat && inhibitor->surface == focused;
           });
  }

  bool Server::clientShortcutsInhibitAllowed(wlr_surface* surface) const {
    View* view = surface != nullptr ? View::fromSurface(wlr_surface_get_root_surface(surface)) : nullptr;
    return view == nullptr || view->allowsShortcutsInhibit();
  }

  void Server::refreshKeyboardShortcutsInhibitors() {
    for (const auto& watch : m_shortcutsInhibitors) {
      wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = watch->inhibitor;
      const bool active = watch->requestedActive && clientShortcutsInhibitAllowed(inhibitor->surface);
      if (active && !inhibitor->active) {
        wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
      } else if (!active && inhibitor->active) {
        wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inhibitor);
      }
    }
  }

  bool Server::toggleKeyboardShortcutsInhibit() {
    if (m_seat == nullptr || m_sessionLocked) {
      return false;
    }
    wlr_seat* seat = m_seat->wlr();
    wlr_surface* focused = seat->keyboard_state.focused_surface;
    if (focused == nullptr) {
      return false;
    }

    const bool enable = !keyboardShortcutsInhibited();
    // A client may have created an inhibitor after manual passthrough began.
    // Disable both sources together so a single escape always restores binds.
    m_manualShortcutsInhibit.disable(focused);
    bool clientInhibitor = false;
    for (const auto& watch : m_shortcutsInhibitors) {
      wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = watch->inhibitor;
      if (inhibitor->seat != seat || inhibitor->surface != focused) {
        continue;
      }
      watch->requestedActive = enable;
      clientInhibitor = clientShortcutsInhibitAllowed(focused);
    }
    refreshKeyboardShortcutsInhibitors();
    if (enable && !clientInhibitor) {
      m_manualShortcutsInhibit.enable(focused);
    }
    return true;
  }

  const Keybind* Server::matchKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) const {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const uint32_t lowered = xkb_keysym_to_lower(keysym);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        // Allow `submap reset` from the default context to always match, so users
        // can define a global emergency exit.
        if (!m_activeSubmaps.empty() && bind.submap.empty() && isSubmapResetBind(bind)) {
          // Fall through to match below.
        } else {
          continue;
        }
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (keyboardShortcutsInhibited() && !bind.allowWhenInhibited) {
        continue;
      }
      if (bind.modifierOnly) {
        continue;
      }
      if (bind.wheel != WheelDirection::None || bind.mouseButton != 0) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected || (lowered != bind.keysym && rawKeysym != bind.keysym)) {
        continue;
      }
      return &bind;
    }

    return nullptr;
  }

  void Server::refreshModTapState() {
    const uint32_t modifiers = keyboardModifiers();
    const bool held = (modifiers & modKey()) != 0;
    if (held && !m_modHeld) {
      const uint32_t plain = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2 | modKey());
      m_modTapPristine = plain == 0;
      // Defer the whole hold presentation so a clean tap can open the launcher
      // without flashing the shade or stealing a game's pointer capture.
      if (m_modHoldTimer == nullptr) {
        m_modHoldTimer = wl_event_loop_add_timer(wl_display_get_event_loop(m_display), onModHoldTimer, this);
      }
      if (m_modHoldTimer != nullptr) {
        // 0 disarms a Wayland timer, so keep at least one millisecond even if
        // the configured value is at its lower bound.
        wl_event_source_timer_update(m_modHoldTimer, std::max(1, config().input.modHoldDelayMs));
      }
    } else if (!held && m_modHeld) {
      if (m_modHoldTimer != nullptr) {
        wl_event_source_timer_update(m_modHoldTimer, 0);
      }
      setModHoldActive(false);
    }
    m_modHeld = held;
  }

  void Server::setModHoldActive(bool active) {
    // Holding Mod makes the desktop a window-management surface: window
    // content stops receiving pointer input, and only the window under the
    // pointer gets the mockup's blur and subtle veil. Native chrome and panels
    // stay live and sharp.
    if (active != m_modInputShield) {
      m_modInputShield = active;
      if (m_cursor != nullptr) {
        m_cursor->setModInputShield(active);
      }
    }
    updateOverlayChrome();

    if (!active) {
      updateModHoverBlur(nullptr);
      return;
    }
    if (m_cursor != nullptr && !m_sessionLocked) {
      LayerSurface* layer = nullptr;
      View* hovered = hoverViewAt(m_cursor->wlr()->x, m_cursor->wlr()->y, &layer);
      updateModHoverBlur(layer == nullptr ? hovered : nullptr);
    } else {
      updateModHoverBlur(nullptr);
    }
  }

  void Server::updateOverlayChrome() {
    const bool shown = overlayChromeActive() && !m_sessionLocked;
    // Resize handles are native too: dots on the focused floating window and
    // dividers between mosaics, drawn inside each window tree so they animate
    // with it instead of trailing on an external overlay.
    for (const auto& view : views()) {
      if (view != nullptr) {
        view->setModHandlesActive(shown);
        // Overlay title bars are native chrome: they appear over the content
        // without changing any window geometry.
        view->setTitleBarOverlay(shown && config().appearance.titleBar.overlay);
      }
    }
    m_titleBarOverlayVisible = shown && config().appearance.titleBar.overlay;
  }

  void Server::setOverlayPinned(bool pinned) {
    if (m_overlayPinned == pinned) {
      return;
    }
    m_overlayPinned = pinned;
    updateOverlayChrome();
  }

  View* Server::hoverViewAt(double lx, double ly, LayerSurface** layer) {
    wlr_surface* surface = nullptr;
    double sx = 0;
    double sy = 0;
    LayerSurface* hitLayer = nullptr;
    View* view = viewAt(lx, ly, &surface, &sx, &sy, &hitLayer);
    // The title bar and its buttons are native cairo buffers, not a
    // wlr_scene_surface, so viewAt cannot see them. The SSD hit test knows the
    // whole frame; use it as a fallback for the focus-by-hover gesture.
    if (view == nullptr && hitLayer == nullptr) {
      if (const auto hit = ssdFrameAt(lx, ly); hit.has_value()) {
        view = hit->view;
      }
    }
    if (layer != nullptr) {
      *layer = hitLayer;
    }
    return view;
  }

  void Server::updateModHoverBlur(View* view) {
    if (!m_modInputShield || m_sessionLocked || view == nullptr || !view->mapped()) {
      view = nullptr;
    }
    if (view != m_modHoverView) {
      if (m_modHoverView != nullptr) {
        m_modHoverView->setModHover(false);
      }
      m_modHoverView = view;
      if (m_modHoverView != nullptr) {
        m_modHoverView->setModHover(true);
      }
    }
    if (m_modHoverView != nullptr) {
      // Mantener Super es también un modo de enfoque: la ventana bajo el
      // puntero pasa a ser la que recibirá el teclado, no la última que tenía
      // el foco. Se comprueba aunque la ventana ya estuviera bajo el puntero:
      // un panel del shell puede haber tomado el teclado mientras tanto y,
      // sin esto, el panel no se cerraba ni la ventana se enfocaba.
      View* target = m_modHoverView->placementRoot();
      if (target != nullptr) {
        target = target->inputDelegate();
      }
      View* focused = View::fromSurface(seat()->wlr()->keyboard_state.focused_surface);
      if (target != nullptr && target != focused) {
        focusView(m_modHoverView, FocusReason::PointerHover);
      }
    }
  }

  void Server::forgetModHoverView(const View* view) {
    if (m_modHoverView == view) {
      m_modHoverView = nullptr;
    }
  }

  int Server::onModHoldTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    if ((self->keyboardModifiers() & self->modKey()) != 0 && !self->m_sessionLocked) {
      self->setModHoldActive(true);
      // Shell may have just missed the modifier event while starting up, so
      // publish the settled hold state for any shell listening for it.
      if (Ipc* ipc = self->ipc(); ipc != nullptr) {
        ipc->notifyModifiersChanged();
      }
    }
    return 0;
  }

  uint32_t Server::keyboardModifiers(bool includeCaptured) const {
    if (!includeCaptured && m_inputCapture != nullptr && (m_inputCapture->capabilities() & 1U) != 0) {
      return 0;
    }
    uint32_t modifiers = 0;
    for (const auto& keyboard : m_keyboards) {
      if (keyboard == nullptr
          || keyboard->wlr() == nullptr
          || (m_inputMethodRelay != nullptr && m_inputMethodRelay->ownsKeyboard(keyboard->wlr()))) {
        continue;
      }
      // Input-method-owned virtual keyboards mirror events that already ran
      // through shortcut matching. Their last mask can also outlive the grab,
      // so folding it back in can turn a later plain key into a stale chord.
      modifiers |= wlr_keyboard_get_modifiers(keyboard->wlr());
    }
    return modifiers;
  }

  std::optional<Keybind> Server::handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) {
    const Keybind* bind = matchKeybind(keysym, rawKeysym, modifiers);
    if (bind == nullptr) {
      return std::nullopt;
    }
    const Keybind matched = *bind;
    bool cooldownBlocked = false;
    const bool handled = executeAction(matched, nullptr, &cooldownBlocked);
    return handled || cooldownBlocked ? std::optional<Keybind>{matched} : std::nullopt;
  }

  bool Server::handleWheelBind(WheelDirection direction, uint32_t modifiers) {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (bind.wheel != direction) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeAction(bind);
    }

    return false;
  }

  std::optional<Keybind> Server::handleMouseBind(uint32_t button, uint32_t modifiers, bool* actionExecuted) {
    if (actionExecuted != nullptr) {
      *actionExecuted = false;
    }
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      // Non-mouse binds carry 0 here, which never equals a BTN_* code.
      if (bind.mouseButton != canonicalMouseButton(button)) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      bool cooldownBlocked = false;
      const bool handled = executeAction(bind, nullptr, &cooldownBlocked);
      if (!handled && !cooldownBlocked) {
        return std::nullopt;
      }
      if (actionExecuted != nullptr) {
        *actionExecuted = handled;
      }
      return bind;
    }

    return std::nullopt;
  }

  void Server::pushSubmap(const std::string& name) {
    cancelModifierTap();
    m_activeSubmaps.push_back(name);
    notifySubmapChanged();
  }

  void Server::popSubmap() {
    cancelModifierTap();
    if (!m_activeSubmaps.empty()) {
      m_activeSubmaps.pop_back();
      notifySubmapChanged();
    }
  }

} // namespace umbriel
