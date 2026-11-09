// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — заглушка режима апплета без GL. Идея — NX-torrent-player (Apache-2.0): app/main.cpp,
// appletModeBlock; код и текст свои.
#include "platform/applet_mode.hpp"

#include <switch.h>

#include <cstdio>

#include "platform/paths.hpp"

namespace mk::plat {

bool isTitleMode() {
    const AppletType t = appletGetAppletType();
    return t == AppletType_Application || t == AppletType_SystemApplication;
}

int appletTypeNumber() { return static_cast<int>(appletGetAppletType()); }

void runAppletModeBlock() {
    consoleInit(nullptr);
    // Встроенный шрифт консоли libnx — только ASCII: русский дан транслитом.
    std::printf("\n  %s %s\n\n", MK_APP_NAME, MK_APP_VERSION);
    std::printf("  Prilozhenie zapushcheno v rezhime apleta (iz Al'boma).\n");
    std::printf("  Zdes' malo pamyati i net nuzhnykh sluzhb.\n\n");
    std::printf("  Zapustite prilozhenie cherez igru, uderzhivaya R:\n");
    std::printf("    1) na glavnom ekrane vyberite lyubuyu igru;\n");
    std::printf("    2) zazhmite R i, ne otpuskaya, zapustite igru;\n");
    std::printf("    3) v Homebrew Menu vyberite %s.\n\n", MK_APP_NAME);
    std::printf("  ----------------------------------------------\n\n");
    std::printf("  The app was started in applet mode (from the Album).\n");
    std::printf("  Start it through a game while holding R:\n");
    std::printf("    1) pick any game on the HOME screen;\n");
    std::printf("    2) hold R and launch the game;\n");
    std::printf("    3) choose %s in the Homebrew Menu.\n\n", MK_APP_NAME);
    std::printf("  Nazhmite + dlya vykhoda / Press + to exit.\n");
    consoleUpdate(nullptr);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

}  // namespace mk::plat
