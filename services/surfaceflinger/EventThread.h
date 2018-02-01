/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <android-base/thread_annotations.h>

#include <gui/DisplayEventReceiver.h>
#include <gui/IDisplayEventConnection.h>
#include <private/gui/BitTube.h>

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/SortedVector.h>

#include "DisplayDevice.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------

class SurfaceFlinger;
class String8;

// ---------------------------------------------------------------------------

class VSyncSource : public virtual RefBase {
public:
    class Callback : public virtual RefBase {
    public:
        virtual ~Callback() {}
        virtual void onVSyncEvent(nsecs_t when) = 0;
    };

    virtual ~VSyncSource() {}
    virtual void setVSyncEnabled(bool enable) = 0;
    virtual void setCallback(const sp<Callback>& callback) = 0;
    virtual void setPhaseOffset(nsecs_t phaseOffset) = 0;
};

class EventThread : public virtual RefBase, private VSyncSource::Callback {
    class Connection : public BnDisplayEventConnection {
    public:
        explicit Connection(const sp<EventThread>& eventThread);
        status_t postEvent(const DisplayEventReceiver::Event& event);

        // count >= 1 : continuous event. count is the vsync rate
        // count == 0 : one-shot event that has not fired
        // count ==-1 : one-shot event that fired this round / disabled
        int32_t count;

    private:
        virtual ~Connection();
        virtual void onFirstRef();
        status_t stealReceiveChannel(gui::BitTube* outChannel) override;
        status_t setVsyncRate(uint32_t count) override;
        void requestNextVsync() override; // asynchronous
        sp<EventThread> const mEventThread;
        gui::BitTube mChannel;
    };

public:
    EventThread(const sp<VSyncSource>& src, SurfaceFlinger& flinger, bool interceptVSyncs,
                const char* threadName);
    ~EventThread();

    sp<Connection> createEventConnection() const;
    status_t registerDisplayEventConnection(const sp<Connection>& connection);

    void setVsyncRate(uint32_t count, const sp<Connection>& connection);
    void requestNextVsync(const sp<Connection>& connection);

    // called before the screen is turned off from main thread
    void onScreenReleased();

    // called after the screen is turned on from main thread
    void onScreenAcquired();

    // called when receiving a hotplug event
    void onHotplugReceived(int type, bool connected);

    void dump(String8& result) const;

    void setPhaseOffset(nsecs_t phaseOffset);

private:
    void threadMain();
    Vector<sp<EventThread::Connection>> waitForEventLocked(std::unique_lock<std::mutex>* lock,
                                                           DisplayEventReceiver::Event* event)
            REQUIRES(mMutex);

    void removeDisplayEventConnection(const wp<Connection>& connection);
    void enableVSyncLocked() REQUIRES(mMutex);
    void disableVSyncLocked() REQUIRES(mMutex);

    // Implements VSyncSource::Callback
    void onVSyncEvent(nsecs_t timestamp) override;

    // constants
    sp<VSyncSource> mVSyncSource GUARDED_BY(mMutex);
    SurfaceFlinger& mFlinger;

    std::thread mThread;
    mutable std::mutex mMutex;
    mutable std::condition_variable mCondition;

    // protected by mLock
    SortedVector<wp<Connection>> mDisplayEventConnections GUARDED_BY(mMutex);
    Vector<DisplayEventReceiver::Event> mPendingEvents GUARDED_BY(mMutex);
    DisplayEventReceiver::Event mVSyncEvent[DisplayDevice::NUM_BUILTIN_DISPLAY_TYPES] GUARDED_BY(
            mMutex);
    bool mUseSoftwareVSync GUARDED_BY(mMutex) = false;
    bool mVsyncEnabled GUARDED_BY(mMutex) = false;
    bool mKeepRunning GUARDED_BY(mMutex) = true;

    // for debugging
    bool mDebugVsyncEnabled GUARDED_BY(mMutex) = false;

    const bool mInterceptVSyncs = false;
};

// ---------------------------------------------------------------------------

}; // namespace android
