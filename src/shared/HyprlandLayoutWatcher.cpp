#include "HyprlandLayoutWatcher.hpp"

#include "../core/PortalManager.hpp"
#include "../includes.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static std::string runtimeDir() {
    const char* XDG = getenv("XDG_RUNTIME_DIR");
    if (XDG && *XDG)
        return XDG;

    return "/run/user/" + std::to_string(getuid());
}

static std::string hyprlandInstanceDir() {
    const char* HIS = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (HIS && *HIS)
        return runtimeDir() + "/hypr/" + HIS;

    // No signature in our environment: there is one compositor per user session,
    // so take the first instance directory that actually has the socket.
    std::error_code ec;
    for (const auto& ENTRY : std::filesystem::directory_iterator(runtimeDir() + "/hypr", ec)) {
        if (std::filesystem::exists(ENTRY.path() / ".socket2.sock", ec))
            return ENTRY.path();
    }

    return "";
}

// The devices reply is a flat JSON document; we only need the first keyboard's
// active keymap and do not want to pull in a JSON dependency for that.
static std::optional<std::string> firstActiveKeymap(const std::string& json) {
    static constexpr const char* KEY   = "\"active_keymap\"";
    const auto                   FIELD = json.find(KEY);
    if (FIELD == std::string::npos)
        return std::nullopt;
    const auto COLON = json.find(':', FIELD);
    if (COLON == std::string::npos)
        return std::nullopt;
    const auto OPEN = json.find('"', COLON);
    if (OPEN == std::string::npos)
        return std::nullopt;
    const auto CLOSE = json.find('"', OPEN + 1);
    if (CLOSE == std::string::npos)
        return std::nullopt;

    return json.substr(OPEN + 1, CLOSE - OPEN - 1);
}

CHyprlandLayoutWatcher::~CHyprlandLayoutWatcher() {
    // The fd is closed on exit, but do not touch the event loop from here: the
    // manager may already be tearing down.
    if (m_iFd >= 0)
        close(m_iFd);
}

void CHyprlandLayoutWatcher::start() {
    if (m_iFd >= 0)
        return;

    if (!connectSocket())
        return;

    seedActiveLayout();

    g_pPortalManager->addFdToEventLoop(m_iFd, POLLIN, [this] { onEvent(); });
}

void CHyprlandLayoutWatcher::stop() {
    if (m_iFd < 0)
        return;

    g_pPortalManager->removeFdFromEventLoop(m_iFd);
    close(m_iFd);
    m_iFd = -1;
}

bool CHyprlandLayoutWatcher::connectSocket() {
    m_sInstanceDir = hyprlandInstanceDir();
    if (m_sInstanceDir.empty()) {
        Debug::log(WARN, "[layout] no Hyprland IPC socket found, keyboard layout mirroring disabled");
        return false;
    }

    const auto PATH = m_sInstanceDir + "/.socket2.sock";

    const int  FD = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (FD < 0) {
        Debug::log(WARN, "[layout] could not create IPC socket: {}", strerror(errno));
        return false;
    }

    sockaddr_un addr{.sun_family = AF_UNIX};
    if (PATH.size() >= sizeof(addr.sun_path)) {
        Debug::log(WARN, "[layout] IPC socket path too long: {}", PATH);
        close(FD);
        return false;
    }

    strncpy(addr.sun_path, PATH.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(FD, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Debug::log(WARN, "[layout] could not connect to {}: {}", PATH, strerror(errno));
        close(FD);
        return false;
    }

    const int FLAGS = fcntl(FD, F_GETFL, 0);
    fcntl(FD, F_SETFL, FLAGS | O_NONBLOCK);

    static constexpr const char* SUBSCRIBE = "subscribe\nactivelayout\n";
    if (write(FD, SUBSCRIBE, strlen(SUBSCRIBE)) < 0) {
        Debug::log(WARN, "[layout] could not subscribe to activelayout: {}", strerror(errno));
        close(FD);
        return false;
    }

    m_iFd = FD;
    Debug::log(LOG, "[layout] watching Hyprland active layout at {}", PATH);
    return true;
}

bool CHyprlandLayoutWatcher::seedActiveLayout() {
    // activelayout events fire on change only, and the portal may start after the
    // user already switched: ask the compositor once so the group is right from
    // the first injected key instead of falling back to group 0.
    const auto PATH = m_sInstanceDir + "/.socket.sock";

    const int  FD = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (FD < 0)
        return false;

    timeval timeout{.tv_sec = 0, .tv_usec = 250000};
    setsockopt(FD, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_un addr{.sun_family = AF_UNIX};
    if (PATH.size() >= sizeof(addr.sun_path)) {
        close(FD);
        return false;
    }

    strncpy(addr.sun_path, PATH.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(FD, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(FD);
        return false;
    }

    static constexpr const char* REQUEST = "j/devices";
    if (write(FD, REQUEST, strlen(REQUEST)) < 0) {
        close(FD);
        return false;
    }

    std::string            response;
    std::array<char, 8192> buf;
    while (response.size() < (1U << 20)) {
        const auto N = read(FD, buf.data(), buf.size());
        if (N > 0) {
            response.append(buf.data(), static_cast<size_t>(N));
            continue;
        }
        break; // EOF, timeout or error: parse whatever arrived
    }
    close(FD);

    const auto LAYOUT = firstActiveKeymap(response);
    if (!LAYOUT)
        return false;

    std::lock_guard<std::mutex> lg(m_mMutex);
    if (!m_sActiveLayout.empty())
        return true; // a live event arrived while we were querying; it is fresher

    m_sActiveLayout = *LAYOUT;
    Debug::log(LOG, "[layout] initial active layout '{}'", *LAYOUT);
    return true;
}

void CHyprlandLayoutWatcher::onEvent() {
    std::array<char, 4096> buf;
    while (true) {
        const auto N = read(m_iFd, buf.data(), buf.size());
        if (N > 0) {
            m_sBuffer.append(buf.data(), static_cast<size_t>(N));
            continue;
        }
        if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (N < 0 && errno == EINTR)
            continue;

        // EOF or a real error: the compositor is gone or restarting. Drop the
        // socket; if a fresh instance appears it will have a new signature and
        // the portal restarts with the session anyway.
        Debug::log(WARN, "[layout] Hyprland IPC closed ({})", N == 0 ? "EOF" : strerror(errno));
        stop();
        return;
    }

    bool changed = false;
    while (true) {
        const auto POS = m_sBuffer.find('\n');
        if (POS == std::string::npos)
            break;

        const auto LINE = m_sBuffer.substr(0, POS);
        m_sBuffer.erase(0, POS + 1);

        static constexpr const char* PREFIX = "activelayout>>";
        if (!LINE.starts_with(PREFIX))
            continue;

        const auto COMMA = LINE.find(',', strlen(PREFIX));
        if (COMMA == std::string::npos)
            continue;

        const auto LAYOUT = LINE.substr(COMMA + 1);
        {
            std::lock_guard<std::mutex> lg(m_mMutex);
            if (LAYOUT == m_sActiveLayout)
                continue;
            m_sActiveLayout = LAYOUT;
        }

        Debug::log(LOG, "[layout] active layout is now '{}'", LAYOUT);
        changed = true;
    }

    if (changed)
        dispatchChange();
}

void CHyprlandLayoutWatcher::dispatchChange() {
    std::vector<std::function<void()>> listeners;
    {
        std::lock_guard<std::mutex> lg(m_mMutex);
        for (const auto& [_, FN] : m_vListeners)
            listeners.emplace_back(FN);
    }

    for (const auto& FN : listeners)
        FN();
}

std::optional<xkb_layout_index_t> CHyprlandLayoutWatcher::activeGroup(struct xkb_keymap* keymap) const {
    std::lock_guard<std::mutex> lg(m_mMutex);
    if (!keymap || m_sActiveLayout.empty())
        return std::nullopt;

    for (xkb_layout_index_t i = 0; i < xkb_keymap_num_layouts(keymap); ++i) {
        const char* NAME = xkb_keymap_layout_get_name(keymap, i);
        if (NAME && m_sActiveLayout == NAME)
            return i;
    }

    return std::nullopt;
}

size_t CHyprlandLayoutWatcher::addChangeListener(std::function<void()> fn) {
    std::lock_guard<std::mutex> lg(m_mMutex);
    m_vListeners.emplace_back(m_iNextListenerId, std::move(fn));
    return m_iNextListenerId++;
}

void CHyprlandLayoutWatcher::removeChangeListener(size_t id) {
    if (!id)
        return;

    std::lock_guard<std::mutex> lg(m_mMutex);
    std::erase_if(m_vListeners, [id](const auto& entry) { return entry.first == id; });
}
