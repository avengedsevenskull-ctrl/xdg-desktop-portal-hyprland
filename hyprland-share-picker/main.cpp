#include "Picker.hpp"
#include "PickerData.hpp"

#include <hyprtoolkit/core/Backend.hpp>
#include <hyprtoolkit/element/Button.hpp>
#include <hyprtoolkit/element/Checkbox.hpp>
#include <hyprtoolkit/element/ColumnLayout.hpp>
#include <hyprtoolkit/element/Null.hpp>
#include <hyprtoolkit/element/Rectangle.hpp>
#include <hyprtoolkit/element/RowLayout.hpp>
#include <hyprtoolkit/element/Text.hpp>
#include <hyprtoolkit/window/Window.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace Hyprtoolkit;
using namespace Hyprutils::Memory;

static CDynamicSize fullSize() {
    return {CDynamicSize::HT_SIZE_PERCENT, CDynamicSize::HT_SIZE_PERCENT, {1.F, 1.F}};
}

static CSharedPointer<CTextElement> makeText(const CSharedPointer<IBackend>& backend, std::string text, CFontSize fontSize = {CFontSize::HT_FONT_TEXT}) {
    return CTextBuilder::begin()->text(std::move(text))->fontSize(std::move(fontSize))->color([backend] { return backend->getPalette()->m_colors.text; })->commence();
}

// Remote-desktop consent gate.
//
// Desktop portals are headless, so the portal backend reuses this picker as the
// consent UI: it runs us with --remote-desktop and passes the request facts
// through the environment. We print [AUTHORIZED] (plus [PERSIST] when the user
// asked to remember the grant) on stdout; ScreencopyShared.cpp parses exactly
// those tokens.
//
// The app ID is caller-controlled, so it is rendered as plain text. Hyprtoolkit
// does not interpret markup, which makes the message immune to HTML spoofing.
static int runRemoteDesktopConsent() {
    const char*       APPID = std::getenv("XDPH_REMOTE_DESKTOP_APP_ID");
    const std::string APP   = APPID && *APPID ? APPID : "An application";

    const char*       DEVICES = std::getenv("XDPH_REMOTE_DESKTOP_DEVICE_TYPES");
    const std::string DEVSTR  = DEVICES ? DEVICES : "";
    const bool        POINTER  = DEVSTR.find("pointer") != std::string::npos;
    const bool        KEYBOARD = DEVSTR.find("keyboard") != std::string::npos;

    std::string what;
    if (POINTER && !KEYBOARD)
        what = "pointer";
    else if (KEYBOARD && !POINTER)
        what = "keyboard";
    else
        what = "pointer and keyboard";

    // Only offer to remember the grant when the app actually asked to persist.
    const char* PERSIST     = std::getenv("XDPH_REMOTE_DESKTOP_PERSIST");
    const bool  CAN_PERSIST = PERSIST && *PERSIST == '1';

    const auto  BACKEND = IBackend::create();
    if (!BACKEND) {
        std::cerr << "[XDPH_PICKER_ERROR] Failed to initialize Hyprtoolkit\n";
        return 1;
    }

    BACKEND->setLogFn([](eLogLevel, const std::string& message) { std::cerr << "[hyprtoolkit] " << message << '\n'; });

    bool authorized = false;
    bool persist    = false;

    const auto WINDOW = CWindowBuilder::begin()
                            ->type(HT_WINDOW_TOPLEVEL)
                            ->preferredSize({520, CAN_PERSIST ? 224 : 192})
                            ->minSize({440, 170})
                            ->resizable(false)
                            ->appTitle("Allow remote control?")
                            ->appClass("hyprland-share-picker")
                            ->commence();
    if (!WINDOW) {
        BACKEND->destroy();
        return 1;
    }

    const auto BACKGROUND = CRectangleBuilder::begin()->color([BACKEND] { return BACKEND->getPalette()->m_colors.background; })->size(fullSize())->commence();
    WINDOW->m_rootElement->addChild(BACKGROUND);

    const auto LAYOUT = CColumnLayoutBuilder::begin()->size(fullSize())->gap(10)->commence();
    LAYOUT->setMargin(14);
    BACKGROUND->addChild(LAYOUT);

    LAYOUT->addChild(makeText(BACKEND, "Allow remote control?", {CFontSize::HT_FONT_H2}));
    LAYOUT->addChild(makeText(BACKEND, APP + " wants to control your " + what + "."));

    CSharedPointer<CCheckboxElement> persistBox;
    if (CAN_PERSIST) {
        const auto ROW = CRowLayoutBuilder::begin()->size({CDynamicSize::HT_SIZE_PERCENT, CDynamicSize::HT_SIZE_AUTO, {1.F, 1.F}})->gap(8)->commence();
        persistBox     = CCheckboxBuilder::begin()->toggled(false)->commence();
        ROW->addChild(persistBox);
        ROW->addChild(makeText(BACKEND, "Allow " + APP + " to skip this prompt in the future"));
        LAYOUT->addChild(ROW);
    }

    const auto ACTIONS = CRowLayoutBuilder::begin()->size({CDynamicSize::HT_SIZE_PERCENT, CDynamicSize::HT_SIZE_AUTO, {1.F, 1.F}})->gap(8)->commence();
    const auto SPACER  = CNullBuilder::begin()->commence();
    SPACER->setGrow(true);
    ACTIONS->addChild(SPACER);
    ACTIONS->addChild(CButtonBuilder::begin()->label("Deny")->onMainClick([&](CSharedPointer<CButtonElement>) {
        WINDOW->close();
        BACKEND->destroy();
    })->commence());
    ACTIONS->addChild(CButtonBuilder::begin()
                          ->label("Allow")
                          ->accent(true)
                          ->onMainClick([&](CSharedPointer<CButtonElement>) {
                              authorized = true;
                              if (persistBox)
                                  persist = persistBox->state();
                              WINDOW->close();
                              BACKEND->destroy();
                          })
                          ->commence());
    LAYOUT->addChild(ACTIONS);

    WINDOW->m_events.closeRequest.listenStatic([&] {
        WINDOW->close();
        BACKEND->destroy();
    });

    WINDOW->open();
    BACKEND->enterLoop();

    if (!authorized)
        return 1;

    std::cout << "[AUTHORIZED]";
    if (persist)
        std::cout << "[PERSIST]";
    std::cout << '\n' << std::flush;
    return 0;
}

int main(int argc, char** argv) {
    setenv("HT_QUIET", "1", 0);

    bool allowTokenByDefault = false;
    bool remoteDesktop       = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--allow-token")
            allowTokenByDefault = true;
        else if (std::string_view{argv[i]} == "--remote-desktop")
            remoteDesktop = true;
    }

    if (remoteDesktop)
        return runRemoteDesktopConsent();

    const auto BACKEND = IBackend::create();
    if (!BACKEND) {
        std::cerr << "[XDPH_PICKER_ERROR] Failed to initialize Hyprtoolkit\n";
        return 1;
    }

    BACKEND->setLogFn([](eLogLevel, const std::string& message) { std::cerr << "[hyprtoolkit] " << message << '\n'; });

    const auto WINDOW_LIST = std::getenv("XDPH_WINDOW_SHARING_LIST");
    const auto OUTPUT_LIST = std::getenv("XDPH_OUTPUT_SHARING_LIST");

    CPicker    picker(BACKEND, parseOutputList(OUTPUT_LIST ? OUTPUT_LIST : ""), parseWindowList(WINDOW_LIST ? WINDOW_LIST : ""), allowTokenByDefault);
    if (!picker.initialize()) {
        std::cerr << "[XDPH_PICKER_ERROR] Failed to create the picker window\n";
        BACKEND->destroy();
        return 1;
    }

    picker.run();
    return 0;
}
