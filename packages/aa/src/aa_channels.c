/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Channel vocabulary implementation.  Static tables only; no allocation.
 */
#include "aa_channels.h"

bool aa_channel_known(uint8_t channel)
{
    return channel <= (uint8_t)AA_CHANNEL_NAVIGATION;
}

const char *aa_channel_name(uint8_t channel)
{
    switch (channel) {
    case AA_CHANNEL_CONTROL:
        return "control";
    case AA_CHANNEL_INPUT:
        return "input";
    case AA_CHANNEL_SENSOR:
        return "sensor";
    case AA_CHANNEL_VIDEO:
        return "video";
    case AA_CHANNEL_MEDIA_AUDIO:
        return "media_audio";
    case AA_CHANNEL_SPEECH_AUDIO:
        return "speech_audio";
    case AA_CHANNEL_SYSTEM_AUDIO:
        return "system_audio";
    case AA_CHANNEL_AV_INPUT:
        return "av_input";
    case AA_CHANNEL_BLUETOOTH:
        return "bluetooth";
    case AA_CHANNEL_NAVIGATION:
        return "navigation";
    }
    return "unknown";
}

const char *aa_feature_name(aa_feature feature)
{
    switch (feature) {
    case AA_FEATURE_EV_ENERGY:
        return "ev_energy";
    case AA_FEATURE_NAVIGATION:
        return "navigation";
    case AA_FEATURE_VIDEO:
        return "video";
    case AA_FEATURE_AUDIO:
        return "audio";
    case AA_FEATURE_INPUT:
        return "input";
    }
    return "unknown";
}
