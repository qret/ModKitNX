// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — перевод mk::evaluate_sdk_status в вид для экрана. См. platform/sdk_compat.hpp.
#include "platform/sdk_compat.hpp"

#include <mk/sdk.hpp>

#include <optional>

#include "platform/log.hpp"

namespace mk::plat {

namespace {

std::optional<mk::Ver3> toCore(const Version3& v) {
    if (!v.valid) return std::nullopt;
    return mk::Ver3{v.major, v.minor, v.micro};
}

Version3 fromCore(const std::optional<mk::Ver3>& v) {
    if (!v) return {};
    return Version3::make(static_cast<uint8_t>((*v)[0]), static_cast<uint8_t>((*v)[1]), static_cast<uint8_t>((*v)[2]));
}

}  // namespace

SdkAdvice adviseSdk(const Version3& gameSdkNca, const Version3& gameSdkMw, const Version3& firmware,
                    bool swappedPresent, bool swappedIsNnSdk, const Version3& swappedVersion,
                    const std::vector<FoundSdk>& found) {
    SdkAdvice a;
    mk::SdkStatusInput in;
    in.game_sdk_nca = toCore(gameSdkNca);
    in.game_sdk_exefs = toCore(gameSdkMw);
    in.firmware = toCore(firmware);
    in.swapped_present = swappedPresent;
    in.swapped_version = toCore(swappedVersion);
    for (const auto& f : found) {
        if (f.version.valid) in.available.push_back(mk::Ver3{f.version.major, f.version.minor, f.version.micro});
    }
    mk::SdkStatus st;
    try {
        st = mk::evaluate_sdk_status(in);
    } catch (const mk::Error& e) {
        MK_LOG("sdk: evaluate failed %s: %s", e.code().c_str(), e.what());
        return a;
    }
    a.gameNeeds = fromCore(st.game_sdk);
    a.bestForFirmware = fromCore(st.recommended ? st.recommended : st.fw_max_sdk);
    a.ruleSource = st.recommended_source;
    if (st.verdict == "not_needed") a.need = SdkNeed::NotNeeded;
    else if (st.verdict == "needed") a.need = SdkNeed::Needed;
    else if (st.verdict == "swapped_ok") a.need = SdkNeed::SwappedOk;
    else if (st.verdict == "swapped_mismatch") a.need = SdkNeed::SwappedTooNew;
    else a.need = SdkNeed::Unknown;
    if (swappedPresent && !swappedIsNnSdk) a.need = SdkNeed::SwappedUnknown;

    if (st.recommended) {
        const Version3 rec = fromCore(st.recommended);
        for (size_t i = 0; i < found.size(); ++i) {
            if (found[i].version == rec) {
                a.recommendedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    return a;
}

}  // namespace mk::plat
