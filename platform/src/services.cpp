// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/services.hpp"

#include <switch.h>

#include <atomic>

#include "platform/log.hpp"
#include "platform/service_lock.hpp"

namespace mk::plat {

namespace {
bool g_ns = false;
bool g_ncm = false;
}  // namespace

bool initServices() {
    ServiceLock lock(serviceMutex());
    Result rc = nsInitialize();
    g_ns = R_SUCCEEDED(rc);
    MK_LOG("services: ns rc=0x%X", rc);
    rc = ncmInitialize();
    g_ncm = R_SUCCEEDED(rc);
    MK_LOG("services: ncm rc=0x%X", rc);
    return g_ns && g_ncm;
}

namespace {
std::atomic<int> g_noSleep{0};
}  // namespace

// Based on TorrentShopNX by Langegen (GPL-3.0): source/utils/switch_utils.cpp (preventSleepBegin/End)
ScopedNoSleep::ScopedNoSleep() {
    if (g_noSleep.fetch_add(1) == 0) {
        if (hosversionAtLeast(5, 0, 0)) appletSetAutoSleepDisabled(true);
        appletSetMediaPlaybackState(true);
        MK_LOG("services: auto-sleep disabled");
    }
}

ScopedNoSleep::~ScopedNoSleep() {
    if (g_noSleep.fetch_sub(1) == 1) {
        if (hosversionAtLeast(5, 0, 0)) appletSetAutoSleepDisabled(false);
        appletSetMediaPlaybackState(false);
        MK_LOG("services: auto-sleep restored");
    }
}

void exitServices() {
    ServiceLock lock(serviceMutex());
    if (g_ncm) ncmExit();
    if (g_ns) nsExit();
    g_ncm = g_ns = false;
}

}  // namespace mk::plat
