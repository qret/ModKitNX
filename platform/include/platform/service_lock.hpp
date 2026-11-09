// SPDX-License-Identifier: GPL-3.0-or-later
// Based on TorrentShopNX by Langegen (GPL-3.0): source/ui/LibraryView.cpp (g_switch_service_mutex)
//
// Один рекурсивный мьютекс на все вызовы ns/ncm/setsys/spl: службы вызываются из фоновых потоков
// (brls::async) и из UI-потока; TorrentShopNX держит такой же мьютекс вокруг каждого шага, а не всего скана.
#pragma once

#include <mutex>

namespace mk::plat {

std::recursive_mutex& serviceMutex();

using ServiceLock = std::lock_guard<std::recursive_mutex>;

}  // namespace mk::plat
