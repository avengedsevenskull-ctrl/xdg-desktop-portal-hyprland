#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <xkbcommon/xkbcommon.h>

// Mirrors the compositor's active keyboard layout.
//
// Wayland gives a portal no way to learn which xkb group the user is currently
// typing in: wl_keyboard.modifiers is only delivered to a client that has
// keyboard focus, and the portal never does. Hyprland announces layout changes
// on its IPC socket, so we subscribe there and let RemoteDesktop stamp injected
// events with the same group the user's own keyboard would produce.
class CHyprlandLayoutWatcher {
  public:
    CHyprlandLayoutWatcher() = default;
    ~CHyprlandLayoutWatcher();

    void start();
    void stop();

    // Resolve the active layout name against a compiled keymap's layout names.
    // Returns nullopt while nothing has been observed yet.
    std::optional<xkb_layout_index_t> activeGroup(struct xkb_keymap* keymap) const;

    // Fired whenever the active layout name changes.
    size_t addChangeListener(std::function<void()> fn);
    void   removeChangeListener(size_t id);

  private:
    bool                                                  connectSocket();
    void                                                  onEvent();
    void                                                  dispatchChange();

    int                                                   m_iFd = -1;
    std::string                                           m_sBuffer;
    mutable std::mutex                                    m_mMutex;
    std::string                                           m_sActiveLayout;
    std::vector<std::pair<size_t, std::function<void()>>> m_vListeners;
    size_t                                                m_iNextListenerId = 1;
};
