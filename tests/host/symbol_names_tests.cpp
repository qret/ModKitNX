// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — хост-тесты читаемых имён функций и деления пробелов sdk на онлайн-службы (source/symbol_names.hpp).
#include <cstdio>
#include <string>

#include "symbol_names.hpp"

using mk::app::onlineServiceOf;
using mk::app::onlineServiceOfSymbol;
using mk::app::readableSymbol;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                                       \
    } while (0)

int main() {
    // Настоящие имена из .dynsym (Gurei на sdk 19.3.13, каталог модуля).
    const std::string inv =
        "_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_37FriendInvitationGameModeDescriptionV2EPKvm";
    const std::string send =
        "_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_"
        "37FriendInvitationGameModeDescriptionV2EPKvm";
    const std::string ldn = "_ZN2nn3ldn11SetProtocolENS0_8ProtocolE";
    const std::string hash = "_ZNSt3__113__hash_memoryEPKvm";

    CHECK(readableSymbol(inv) == "nn::friends::StartFriendInvitation", "friends: StartFriendInvitation");
    CHECK(readableSymbol(send) == "nn::friends::StartSendingFriendInvitation", "friends: StartSendingFriendInvitation");
    CHECK(readableSymbol(ldn) == "nn::ldn::SetProtocol", "ldn: SetProtocol");
    CHECK(readableSymbol(hash) == hash, "std::__1 (St) — как есть");
    CHECK(readableSymbol("_Z3foov") == "foo", "невложенное имя");
    CHECK(readableSymbol("_ZNK2nn2os5Mutex4LockEv") == "nn::os::Mutex::Lock", "const-метод (K)");

    // Кривые и нестандартные — как есть.
    for (const char* bad : {"_ZN2nn99friendsE", "_ZN2nn7friends", "_ZN2nn07friendsE", "_ZN", "_Z", "_ZNE",
                            "_ZN2nn4util8IntrusiveIiE3FooEv", "_ZN2nn3fooC1Ev", "NvRmMemQueryHandleParams",
                            "nnMain", "", "_ZN2n-3fooE"})
        CHECK(readableSymbol(bad) == bad, bad);

    // Онлайн-службы.
    CHECK(onlineServiceOfSymbol(inv) == "friends", "friends — онлайн");
    CHECK(onlineServiceOfSymbol(send) == "friends", "friends (2) — онлайн");
    CHECK(onlineServiceOfSymbol(ldn).empty(), "ldn — локальная игра, не онлайн");
    CHECK(onlineServiceOfSymbol(hash).empty(), "libc++ — не онлайн");
    CHECK(onlineServiceOfSymbol("NvRmMemQueryHandleParams").empty(), "не C++ — не онлайн");
    CHECK(onlineServiceOf("nn::account::EnsureNetworkServiceAccountAvailable") == "account", "аккаунт в сети");
    CHECK(onlineServiceOf("nn::account::LoadNintendoAccountInfo") == "account", "Nintendo Account");
    CHECK(onlineServiceOf("nn::account::GetLastOpenedUser").empty(), "выбор пользователя — не онлайн");
    CHECK(onlineServiceOf("nn::olsc::Initialize") == "olsc", "облачные сохранения");
    CHECK(onlineServiceOf("nn::npns::Listen") == "npns", "уведомления");
    CHECK(onlineServiceOf("nn::nex::Core::Foo") == "online_play", "nex");
    CHECK(onlineServiceOf("nn::pia::inet::Foo") == "online_play", "pia::inet");
    CHECK(onlineServiceOf("nn::pia::local::Foo").empty(), "pia::local — локальная игра, не онлайн");
    CHECK(onlineServiceOf("nn::ec::ShowShopPage") == "ec", "eShop");
    CHECK(onlineServiceOf("nn::news::Foo") == "news", "новости");
    CHECK(onlineServiceOf("nn::friendsx::Foo").empty(), "похожее пространство — не онлайн");
    CHECK(onlineServiceOf("nn::nifm::SubmitNetworkRequest").empty(), "nifm — не онлайн");

    std::printf("symbol_names: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
