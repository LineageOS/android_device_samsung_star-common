/*
 * Copyright (C) 2017 The Android Open Source Project
 * Copyright (C) 2018 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

//#define LOG_NDEBUG 0

#define LOG_TAG "PowerInteractionHandler"

#include <android-base/file.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include <utils/Log.h>

#include "InteractionHandler.h"

#define MAX_LENGTH 64

#define MSINSEC 1000L
#define USINMS 1000000L

using android::base::WriteStringToFile;

InteractionHandler::InteractionHandler()
    : mState(INTERACTION_STATE_UNINITIALIZED),
      mWaitMs(100),
      mMinDurationMs(1400),
      mMaxDurationMs(5650),
      mDurationMs(0) {
}

InteractionHandler::~InteractionHandler() {
    Exit();
}

bool InteractionHandler::Init() {
    std::lock_guard<std::mutex> lk(mLock);

    if (mState != INTERACTION_STATE_UNINITIALIZED)
        return true;

    mEventFd = eventfd(0, EFD_NONBLOCK);
    if (mEventFd < 0) {
        ALOGE("Unable to create event fd (%d)", errno);
        return false;
    }

    mState = INTERACTION_STATE_IDLE;
    mThread = std::unique_ptr<std::thread>(
        new std::thread(&InteractionHandler::Routine, this));

    return true;
}

void InteractionHandler::Exit() {
    std::unique_lock<std::mutex> lk(mLock);
    if (mState == INTERACTION_STATE_UNINITIALIZED)
        return;

    AbortWaitLocked();
    mState = INTERACTION_STATE_UNINITIALIZED;
    lk.unlock();

    mCond.notify_all();
    mThread->join();

    close(mEventFd);
}

void InteractionHandler::PerfLock() {
    ALOGV("%s: acquiring perf lock", __func__);
    WriteStringToFile("40", "/dev/stune/top-app/schedtune.boost", false);
    WriteStringToFile("1", "/dev/stune/top-app/schedtune.prefer_perf", false);
}

void InteractionHandler::PerfRel() {
    ALOGV("%s: releasing perf lock", __func__);
    WriteStringToFile("15", "/dev/stune/top-app/schedtune.boost", false);
    WriteStringToFile("0", "/dev/stune/top-app/schedtune.prefer_perf", false);
}

long long InteractionHandler::CalcTimespecDiffMs(struct timespec start,
                                               struct timespec end) {
    long long diff_in_us = 0;
    diff_in_us += (end.tv_sec - start.tv_sec) * MSINSEC;
    diff_in_us += (end.tv_nsec - start.tv_nsec) / USINMS;
    return diff_in_us;
}

void InteractionHandler::Acquire(int32_t duration) {
    std::lock_guard<std::mutex> lk(mLock);
    if (mState == INTERACTION_STATE_UNINITIALIZED) {
        ALOGW("%s: called while uninitialized", __func__);
        return;
    }

    int inputDuration = duration + 650;
    int finalDuration;
    if (inputDuration > mMaxDurationMs)
        finalDuration = mMaxDurationMs;
    else if (inputDuration > mMinDurationMs)
        finalDuration = inputDuration;
    else
        finalDuration = mMinDurationMs;

    struct timespec cur_timespec;
    clock_gettime(CLOCK_MONOTONIC, &cur_timespec);
    if (mState != INTERACTION_STATE_IDLE && finalDuration <= mDurationMs) {
        long long elapsed_time = CalcTimespecDiffMs(mLastTimespec, cur_timespec);
        // don't hint if previous hint's duration covers this hint's duration
        if (elapsed_time <= (mDurationMs - finalDuration)) {
            ALOGV("%s: Previous duration (%d) cover this (%d) elapsed: %lld",
                  __func__, mDurationMs, finalDuration, elapsed_time);
            return;
        }
    }
    mLastTimespec = cur_timespec;
    mDurationMs = finalDuration;

    ALOGV("%s: input: %d final duration: %d", __func__,
          duration, finalDuration);

    if (mState == INTERACTION_STATE_WAITING)
        AbortWaitLocked();
    else if (mState == INTERACTION_STATE_IDLE)
        PerfLock();

    mWaitMs = mDurationMs;
    mState = INTERACTION_STATE_INTERACTION;
    mCond.notify_one();
}

void InteractionHandler::Release() {
    std::lock_guard<std::mutex> lk(mLock);
    if (mState == INTERACTION_STATE_WAITING) {
        PerfRel();
        mState = INTERACTION_STATE_IDLE;
    } else {
        // clear any wait aborts pending in event fd
        uint64_t val;
        ssize_t ret = read(mEventFd, &val, sizeof(val));

        ALOGW_IF(ret < 0, "%s: failed to clear eventfd (%zd, %d)",
                 __func__, ret, errno);
    }
}

// should be called while locked
void InteractionHandler::AbortWaitLocked() {
    uint64_t val = 1;
    ssize_t ret = write(mEventFd, &val, sizeof(val));
    if (ret != sizeof(val))
        ALOGW("Unable to write to event fd (%zd)", ret);
}

void InteractionHandler::WaitForIdle(int32_t wait_ms) {
    ssize_t ret;
    struct pollfd pfd[1];

    ALOGV("%s: wait:%d", __func__, wait_ms);

    pfd[0].fd = mEventFd;
    pfd[0].events = POLLIN;

    ret = poll(pfd, 1, wait_ms);
    if (ret > 0) {
        ALOGV("%s: wait aborted", __func__);
        return;
    } else if (ret < 0) {
        ALOGE("%s: error in poll while waiting", __func__);
        return;
    }

    return;
}

void InteractionHandler::Routine() {
    std::unique_lock<std::mutex> lk(mLock, std::defer_lock);

    while (true) {
        lk.lock();
        mCond.wait(lk, [&] { return mState != INTERACTION_STATE_IDLE; });
        if (mState == INTERACTION_STATE_UNINITIALIZED)
            return;
        mState = INTERACTION_STATE_WAITING;
        lk.unlock();

        WaitForIdle(mWaitMs);
        Release();
    }
}
