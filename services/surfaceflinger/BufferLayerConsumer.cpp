/*
 * Copyright (C) 2010 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "BufferLayerConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include "BufferLayerConsumer.h"
#include "Layer.h"
#include "Scheduler/DispSync.h"

#include <inttypes.h>

#include <cutils/compiler.h>

#include <hardware/hardware.h>

#include <math/mat4.h>

#include <gui/BufferItem.h>
#include <gui/GLConsumer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <private/gui/ComposerService.h>
#include <renderengine/Image.h>
#include <renderengine/RenderEngine.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Trace.h>

namespace android {

// Macros for including the BufferLayerConsumer name in log messages
#define BLC_LOGV(x, ...) ALOGV("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGD(x, ...) ALOGD("[%s] " x, mName.string(), ##__VA_ARGS__)
//#define BLC_LOGI(x, ...) ALOGI("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGW(x, ...) ALOGW("[%s] " x, mName.string(), ##__VA_ARGS__)
#define BLC_LOGE(x, ...) ALOGE("[%s] " x, mName.string(), ##__VA_ARGS__)

static const mat4 mtxIdentity;

BufferLayerConsumer::BufferLayerConsumer(const sp<IGraphicBufferConsumer>& bq,
                                         renderengine::RenderEngine& engine, uint32_t tex,
                                         Layer* layer)
      : ConsumerBase(bq, false),
        mCurrentCrop(Rect::EMPTY_RECT),
        mCurrentTransform(0),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mCurrentFence(Fence::NO_FENCE),
        mCurrentTimestamp(0),
        mCurrentDataSpace(ui::Dataspace::UNKNOWN),
        mCurrentFrameNumber(0),
        mCurrentTransformToDisplayInverse(false),
        mCurrentSurfaceDamage(),
        mCurrentApi(0),
        mDefaultWidth(1),
        mDefaultHeight(1),
        mFilteringEnabled(true),
        mRE(engine),
        mTexName(tex),
        mLayer(layer),
        mCurrentTexture(BufferQueue::INVALID_BUFFER_SLOT) {
    BLC_LOGV("BufferLayerConsumer");

    memcpy(mCurrentTransformMatrix, mtxIdentity.asArray(), sizeof(mCurrentTransformMatrix));

    mConsumer->setConsumerUsageBits(DEFAULT_USAGE_FLAGS);
}

status_t BufferLayerConsumer::setDefaultBufferSize(uint32_t w, uint32_t h) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setDefaultBufferSize: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }
    mDefaultWidth = w;
    mDefaultHeight = h;
    return mConsumer->setDefaultBufferSize(w, h);
}

void BufferLayerConsumer::setContentsChangedListener(const wp<ContentsChangedListener>& listener) {
    setFrameAvailableListener(listener);
    Mutex::Autolock lock(mMutex);
    mContentsChangedListener = listener;
}

status_t BufferLayerConsumer::updateTexImage(BufferRejecter* rejecter, nsecs_t expectedPresentTime,
                                             bool* autoRefresh, bool* queuedBuffer,
                                             uint64_t maxFrameNumber,
                                             const sp<Fence>& releaseFence) {
    ATRACE_CALL();
    BLC_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        BLC_LOGE("updateTexImage: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }

    BufferItem item;

    // Acquire the next buffer.
    // In asynchronous mode the list is guaranteed to be one buffer
    // deep, while in synchronous mode we use the oldest buffer.
    status_t err = acquireBufferLocked(&item, expectedPresentTime, maxFrameNumber);
    if (err != NO_ERROR) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            err = NO_ERROR;
        } else if (err == BufferQueue::PRESENT_LATER) {
            // return the error, without logging
        } else {
            BLC_LOGE("updateTexImage: acquire failed: %s (%d)", strerror(-err), err);
        }
        return err;
    }

    if (autoRefresh) {
        *autoRefresh = item.mAutoRefresh;
    }

    if (queuedBuffer) {
        *queuedBuffer = item.mQueuedBuffer;
    }

    // We call the rejecter here, in case the caller has a reason to
    // not accept this buffer.  This is used by SurfaceFlinger to
    // reject buffers which have the wrong size
    int slot = item.mSlot;
    if (rejecter && rejecter->reject(mSlots[slot].mGraphicBuffer, item)) {
        releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
        return BUFFER_REJECTED;
    }

    // Release the previous buffer.
    err = updateAndReleaseLocked(item, &mPendingRelease, releaseFence);
    if (err != NO_ERROR) {
        return err;
    }

    if (!mRE.useNativeFenceSync()) {
        // Bind the new buffer to the GL texture.
        //
        // Older devices require the "implicit" synchronization provided
        // by glEGLImageTargetTexture2DOES, which this method calls.  Newer
        // devices will either call this in Layer::onDraw, or (if it's not
        // a GL-composited layer) not at all.
        err = bindTextureImageLocked();
    }

    return err;
}

status_t BufferLayerConsumer::bindTextureImage() {
    Mutex::Autolock lock(mMutex);
    return bindTextureImageLocked();
}

void BufferLayerConsumer::setReleaseFence(const sp<Fence>& fence) {
    if (!fence->isValid()) {
        return;
    }

    auto slot = mPendingRelease.isPending ? mPendingRelease.currentTexture : mCurrentTexture;
    if (slot == BufferQueue::INVALID_BUFFER_SLOT) {
        return;
    }

    auto buffer = mPendingRelease.isPending ? mPendingRelease.graphicBuffer : mCurrentTextureBuffer;
    auto err = addReleaseFence(slot, buffer, fence);
    if (err != OK) {
        BLC_LOGE("setReleaseFence: failed to add the fence: %s (%d)", strerror(-err), err);
    }
}

bool BufferLayerConsumer::releasePendingBuffer() {
    if (!mPendingRelease.isPending) {
        BLC_LOGV("Pending buffer already released");
        return false;
    }
    BLC_LOGV("Releasing pending buffer");
    Mutex::Autolock lock(mMutex);
    status_t result =
            releaseBufferLocked(mPendingRelease.currentTexture, mPendingRelease.graphicBuffer);
    if (result < NO_ERROR) {
        BLC_LOGE("releasePendingBuffer failed: %s (%d)", strerror(-result), result);
    }
    mPendingRelease = PendingRelease();
    return true;
}

sp<Fence> BufferLayerConsumer::getPrevFinalReleaseFence() const {
    Mutex::Autolock lock(mMutex);
    return ConsumerBase::mPrevFinalReleaseFence;
}

status_t BufferLayerConsumer::acquireBufferLocked(BufferItem* item, nsecs_t presentWhen,
                                                  uint64_t maxFrameNumber) {
    status_t err = ConsumerBase::acquireBufferLocked(item, presentWhen, maxFrameNumber);
    if (err != NO_ERROR) {
        return err;
    }

    // If item->mGraphicBuffer is not null, this buffer has not been acquired
    // before.
    if (item->mGraphicBuffer != nullptr) {
        mImages[item->mSlot] = nullptr;
    }

    return NO_ERROR;
}

status_t BufferLayerConsumer::updateAndReleaseLocked(const BufferItem& item,
                                                     PendingRelease* pendingRelease,
                                                     const sp<Fence>& releaseFence) {
    status_t err = NO_ERROR;

    int slot = item.mSlot;

    // Do whatever sync ops we need to do before releasing the old slot.
    if (slot != mCurrentTexture) {
        err = syncForReleaseLocked(releaseFence);
        if (err != NO_ERROR) {
            // Release the buffer we just acquired.  It's not safe to
            // release the old buffer, so instead we just drop the new frame.
            // As we are still under lock since acquireBuffer, it is safe to
            // release by slot.
            releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
            return err;
        }
    }

    BLC_LOGV("updateAndRelease: (slot=%d buf=%p) -> (slot=%d buf=%p)", mCurrentTexture,
             mCurrentTextureBuffer != nullptr ? mCurrentTextureBuffer->handle : 0, slot,
             mSlots[slot].mGraphicBuffer->handle);

    // Hang onto the pointer so that it isn't freed in the call to
    // releaseBufferLocked() if we're in shared buffer mode and both buffers are
    // the same.
    sp<GraphicBuffer> nextTextureBuffer = mSlots[slot].mGraphicBuffer;

    // release old buffer
    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (pendingRelease == nullptr) {
            status_t status = releaseBufferLocked(mCurrentTexture, mCurrentTextureBuffer);
            if (status < NO_ERROR) {
                BLC_LOGE("updateAndRelease: failed to release buffer: %s (%d)", strerror(-status),
                         status);
                err = status;
                // keep going, with error raised [?]
            }
        } else {
            pendingRelease->currentTexture = mCurrentTexture;
            pendingRelease->graphicBuffer = mCurrentTextureBuffer;
            pendingRelease->isPending = true;
        }
    }

    // Update the BufferLayerConsumer state.
    mCurrentTexture = slot;
    mCurrentTextureBuffer = nextTextureBuffer;
    mCurrentTextureBufferStaleForGpu = false;
    mCurrentTextureImageFreed = nullptr;
    mCurrentCrop = item.mCrop;
    mCurrentTransform = item.mTransform;
    mCurrentScalingMode = item.mScalingMode;
    mCurrentTimestamp = item.mTimestamp;
    mCurrentDataSpace = static_cast<ui::Dataspace>(item.mDataSpace);
    mCurrentHdrMetadata = item.mHdrMetadata;
    mCurrentFence = item.mFence;
    mCurrentFenceTime = item.mFenceTime;
    mCurrentFrameNumber = item.mFrameNumber;
    mCurrentTransformToDisplayInverse = item.mTransformToDisplayInverse;
    mCurrentSurfaceDamage = item.mSurfaceDamage;
    mCurrentApi = item.mApi;

    computeCurrentTransformMatrixLocked();

    return err;
}

status_t BufferLayerConsumer::bindTextureImageLocked() {
    ATRACE_CALL();

    return mRE.bindExternalTextureBuffer(mTexName, mCurrentTextureBuffer, mCurrentFence, false);
}

status_t BufferLayerConsumer::syncForReleaseLocked(const sp<Fence>& releaseFence) {
    BLC_LOGV("syncForReleaseLocked");

    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (mRE.useNativeFenceSync() && releaseFence != Fence::NO_FENCE) {
            // TODO(alecmouri): fail further upstream if the fence is invalid
            if (!releaseFence->isValid()) {
                BLC_LOGE("syncForReleaseLocked: failed to flush RenderEngine");
                return UNKNOWN_ERROR;
            }
            status_t err =
                    addReleaseFenceLocked(mCurrentTexture, mCurrentTextureBuffer, releaseFence);
            if (err != OK) {
                BLC_LOGE("syncForReleaseLocked: error adding release fence: "
                         "%s (%d)",
                         strerror(-err), err);
                return err;
            }
        }
    }

    return OK;
}

void BufferLayerConsumer::getTransformMatrix(float mtx[16]) {
    Mutex::Autolock lock(mMutex);
    memcpy(mtx, mCurrentTransformMatrix, sizeof(mCurrentTransformMatrix));
}

void BufferLayerConsumer::setFilteringEnabled(bool enabled) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setFilteringEnabled: BufferLayerConsumer is abandoned!");
        return;
    }
    bool needsRecompute = mFilteringEnabled != enabled;
    mFilteringEnabled = enabled;

    if (needsRecompute && mCurrentTextureBuffer == nullptr) {
        BLC_LOGD("setFilteringEnabled called with mCurrentTextureBuffer == nullptr");
    }

    if (needsRecompute && mCurrentTextureBuffer != nullptr) {
        computeCurrentTransformMatrixLocked();
    }
}

void BufferLayerConsumer::computeCurrentTransformMatrixLocked() {
    BLC_LOGV("computeCurrentTransformMatrixLocked");
    if (mCurrentTextureBuffer == nullptr) {
        BLC_LOGD("computeCurrentTransformMatrixLocked: "
                 "mCurrentTextureBuffer is nullptr");
    }
    GLConsumer::computeTransformMatrix(mCurrentTransformMatrix, mCurrentTextureBuffer, mCurrentCrop,
                                       mCurrentTransform, mFilteringEnabled);
}

nsecs_t BufferLayerConsumer::getTimestamp() {
    BLC_LOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

ui::Dataspace BufferLayerConsumer::getCurrentDataSpace() {
    BLC_LOGV("getCurrentDataSpace");
    Mutex::Autolock lock(mMutex);
    return mCurrentDataSpace;
}

const HdrMetadata& BufferLayerConsumer::getCurrentHdrMetadata() const {
    BLC_LOGV("getCurrentHdrMetadata");
    Mutex::Autolock lock(mMutex);
    return mCurrentHdrMetadata;
}

uint64_t BufferLayerConsumer::getFrameNumber() {
    BLC_LOGV("getFrameNumber");
    Mutex::Autolock lock(mMutex);
    return mCurrentFrameNumber;
}

bool BufferLayerConsumer::getTransformToDisplayInverse() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransformToDisplayInverse;
}

const Region& BufferLayerConsumer::getSurfaceDamage() const {
    return mCurrentSurfaceDamage;
}

int BufferLayerConsumer::getCurrentApi() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentApi;
}

sp<GraphicBuffer> BufferLayerConsumer::getCurrentBuffer(int* outSlot, sp<Fence>* outFence) const {
    Mutex::Autolock lock(mMutex);

    if (outSlot != nullptr) {
        *outSlot = mCurrentTexture;
    }

    if (outFence != nullptr) {
        *outFence = mCurrentFence;
    }

    return mCurrentTextureBuffer;
}

bool BufferLayerConsumer::getAndSetCurrentBufferCacheHint() {
    Mutex::Autolock lock(mMutex);
    bool useCache = mCurrentTextureBufferStaleForGpu;
    // Set the staleness bit here, as this function is only called during a
    // client composition path.
    mCurrentTextureBufferStaleForGpu = true;
    return useCache;
}

Rect BufferLayerConsumer::getCurrentCrop() const {
    Mutex::Autolock lock(mMutex);
    return (mCurrentScalingMode == NATIVE_WINDOW_SCALING_MODE_SCALE_CROP)
            ? GLConsumer::scaleDownCrop(mCurrentCrop, mDefaultWidth, mDefaultHeight)
            : mCurrentCrop;
}

uint32_t BufferLayerConsumer::getCurrentTransform() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransform;
}

uint32_t BufferLayerConsumer::getCurrentScalingMode() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentScalingMode;
}

sp<Fence> BufferLayerConsumer::getCurrentFence() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFence;
}

std::shared_ptr<FenceTime> BufferLayerConsumer::getCurrentFenceTime() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFenceTime;
}

status_t BufferLayerConsumer::doFenceWaitLocked() const {
    if (mCurrentFence->isValid()) {
        if (mRE.useWaitSync()) {
            base::unique_fd fenceFd(mCurrentFence->dup());
            if (fenceFd == -1) {
                BLC_LOGE("doFenceWait: error dup'ing fence fd: %d", errno);
                return -errno;
            }
            if (!mRE.waitFence(std::move(fenceFd))) {
                BLC_LOGE("doFenceWait: failed to wait on fence fd");
                return UNKNOWN_ERROR;
            }
        } else {
            status_t err = mCurrentFence->waitForever("BufferLayerConsumer::doFenceWaitLocked");
            if (err != NO_ERROR) {
                BLC_LOGE("doFenceWait: error waiting for fence: %d", err);
                return err;
            }
        }
    }

    return NO_ERROR;
}

void BufferLayerConsumer::freeBufferLocked(int slotIndex) {
    BLC_LOGV("freeBufferLocked: slotIndex=%d", slotIndex);
    if (slotIndex == mCurrentTexture) {
        mCurrentTexture = BufferQueue::INVALID_BUFFER_SLOT;
        mCurrentTextureImageFreed = std::move(mImages[slotIndex]);
    } else {
        mImages[slotIndex] = nullptr;
    }
    ConsumerBase::freeBufferLocked(slotIndex);
}

void BufferLayerConsumer::onDisconnect() {
    sp<Layer> l = mLayer.promote();
    if (l.get()) {
        l->onDisconnect();
    }
}

void BufferLayerConsumer::onSidebandStreamChanged() {
    FrameAvailableListener* unsafeFrameAvailableListener = nullptr;
    {
        Mutex::Autolock lock(mFrameAvailableMutex);
        unsafeFrameAvailableListener = mFrameAvailableListener.unsafe_get();
    }
    sp<ContentsChangedListener> listener;
    { // scope for the lock
        Mutex::Autolock lock(mMutex);
        ALOG_ASSERT(unsafeFrameAvailableListener == mContentsChangedListener.unsafe_get());
        listener = mContentsChangedListener.promote();
    }

    if (listener != nullptr) {
        listener->onSidebandStreamChanged();
    }
}

void BufferLayerConsumer::addAndGetFrameTimestamps(const NewFrameEventsEntry* newTimestamps,
                                                   FrameEventHistoryDelta* outDelta) {
    sp<Layer> l = mLayer.promote();
    if (l.get()) {
        l->addAndGetFrameTimestamps(newTimestamps, outDelta);
    }
}

void BufferLayerConsumer::abandonLocked() {
    BLC_LOGV("abandonLocked");
    mCurrentTextureBuffer.clear();
    ConsumerBase::abandonLocked();
}

status_t BufferLayerConsumer::setConsumerUsageBits(uint64_t usage) {
    return ConsumerBase::setConsumerUsageBits(usage | DEFAULT_USAGE_FLAGS);
}

void BufferLayerConsumer::dumpLocked(String8& result, const char* prefix) const {
    result.appendFormat("%smTexName=%d mCurrentTexture=%d\n"
                        "%smCurrentCrop=[%d,%d,%d,%d] mCurrentTransform=%#x\n",
                        prefix, mTexName, mCurrentTexture, prefix, mCurrentCrop.left,
                        mCurrentCrop.top, mCurrentCrop.right, mCurrentCrop.bottom,
                        mCurrentTransform);

    ConsumerBase::dumpLocked(result, prefix);
}

}; // namespace android
