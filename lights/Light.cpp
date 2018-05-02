/*
 * Copyright (C) 2018 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.light@2.0-service.9810"

#include "Light.h"

#include <log/log.h>

namespace {
using android::hardware::light::V2_0::LightState;

static int scaleBrightness(int brightness) {
    return brightness * 44800 / 255;
}

static uint32_t rgbToBrightness(const LightState& state) {
    uint32_t color = state.color & 0x00ffffff;
    return ((77 * ((color >> 16) & 0xff)) + (150 * ((color >> 8) & 0xff)) +
            (29 * (color & 0xff))) >> 8;
}

static bool isLit(const LightState& state) {
    return (state.color & 0x00ffffff);
}
} // anonymous namespace

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

Light::Light(std::ofstream&& backlight, std::ofstream&& indicator) :
    mBacklight(std::move(backlight)),
    mIndicator(std::move(indicator)) {
    auto backlightFn(std::bind(&Light::setBacklight, this, std::placeholders::_1));
    auto indicatorFn(std::bind(&Light::setIndicatorLight, this, std::placeholders::_1));
    mLights.emplace(std::make_pair(Type::BACKLIGHT, backlightFn));
    mLights.emplace(std::make_pair(Type::NOTIFICATIONS, indicatorFn));
    mLights.emplace(std::make_pair(Type::ATTENTION, indicatorFn));
    mLights.emplace(std::make_pair(Type::BATTERY, indicatorFn));
}

// Methods from ::android::hardware::light::V2_0::ILight follow.
Return<Status> Light::setLight(Type type, const LightState& state) {
    if (mLights.find(type) != mLights.end()) {
        mLights.at(type)(state);
        return Status::SUCCESS;
    }
    return Status::LIGHT_NOT_SUPPORTED;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    Type *types = new Type[mLights.size()];
    int idx = 0;

    for (auto const &kv : mLights) {
        Type t = kv.first;
        types[idx++] = t;
    }

    {
        hidl_vec<Type> hidl_types{};
        hidl_types.setToExternal(types, mLights.size());

        _hidl_cb(hidl_types);
    }

    delete[] types;

    return Void();
}

void Light::setBacklight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    uint32_t brightness = rgbToBrightness(state);
    brightness = scaleBrightness(brightness);

    mBacklight << brightness << std::endl;
}

void Light::setIndicatorLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    if (!isLit(state)) {
        ALOGD("%s: Lights off", __func__);
        mIndicator << "0x00000000 0 0" << std::endl;
        return;
    }

    uint32_t color = state.color & 0x00ffffff;

    ALOGD("%s: inColor=0x%08x delay_on=%d, delay_off=%d", __func__, color,
          state.flashOnMs, state.flashOffMs);

    char blink[32];
    int count;

    count = snprintf(blink, sizeof(blink) - 1, "0x%08x %d %d", color,
          state.flashOnMs, state.flashOffMs);

    ALOGD("%s: count=%d, blink=\"%s\".", __func__, count, blink);

    mIndicator << blink << std::endl;
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
