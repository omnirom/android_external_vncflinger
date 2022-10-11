#define LOG_TAG "VNCFlinger:AndroidDesktop"
#include <utils/Log.h>

#include <fcntl.h>
#include <sys/eventfd.h>

#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>

#include <rfb/PixelFormat.h>
#include <rfb/Rect.h>
#include <rfb/ScreenSet.h>

#include "AndroidDesktop.h"
#include "AndroidPixelBuffer.h"
#include "InputDevice.h"
#include "VirtualDisplay.h"

using namespace vncflinger;
using namespace android;

AndroidDesktop::AndroidDesktop() {
    mInputDevice = new InputDevice();
    mDisplayRect = Rect(0, 0);

    mEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mEventFd < 0) {
        ALOGE("Failed to create event notifier");
        return;
    }
}

AndroidDesktop::~AndroidDesktop() {
    mInputDevice->stop();
    close(mEventFd);
}

void AndroidDesktop::start(rfb::VNCServer* vs) {
    mServer = vs;

    mPixels = new AndroidPixelBuffer();
    mPixels->setDimensionsChangedListener(this);

    if (updateDisplayInfo() != NO_ERROR) {
        ALOGE("Failed to query display!");
        return;
    }

    ALOGV("Desktop is running");
}

void AndroidDesktop::stop() {
    Mutex::Autolock _L(mLock);

    ALOGV("Shutting down");

    mServer->setPixelBuffer(0);
    mPixels->reset();

    mVirtualDisplay.clear();
    mPixels.clear();
}

void AndroidDesktop::processFrames() {
    Mutex::Autolock _l(mLock);

    updateDisplayInfo();

    // get a frame from the virtual display
    CpuConsumer::LockedBuffer imgBuffer;
    status_t res = mVirtualDisplay->getConsumer()->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        ALOGE("Failed to lock next buffer: %s (%d)", strerror(-res), res);
        return;
    }

    mFrameNumber = imgBuffer.frameNumber;
    //ALOGV("processFrame: [%lu] format: %x (%dx%d, stride=%d)", mFrameNumber, imgBuffer.format,
    //      imgBuffer.width, imgBuffer.height, imgBuffer.stride);

    // we don't know if there was a stride change until we get
    // a buffer from the queue. if it changed, we need to resize

    rfb::Rect bufRect(0, 0, imgBuffer.width, imgBuffer.height);

    // performance is extremely bad if the gpu memory is used
    // directly without copying because it is likely uncached
    mPixels->imageRect(bufRect, imgBuffer.data, imgBuffer.stride);

    mVirtualDisplay->getConsumer()->unlockBuffer(imgBuffer);

    // update clients
    mServer->add_changed(bufRect);
}

// notifies the server loop that we have changes
void AndroidDesktop::notify() {
    static uint64_t notify = 1;
    write(mEventFd, &notify, sizeof(notify));
}

// called when a client resizes the window
unsigned int AndroidDesktop::setScreenLayout(int reqWidth, int reqHeight,
                                             const rfb::ScreenSet& layout) {
    Mutex::Autolock _l(mLock);

    char* dbg = new char[1024];
    layout.print(dbg, 1024);

    ALOGD("setScreenLayout: cur: %s  new: %dx%d", dbg, reqWidth, reqHeight);
    delete[] dbg;

    if (reqWidth == mDisplayRect.getWidth() && reqHeight == mDisplayRect.getHeight()) {
        return rfb::resultInvalid;
    }

    if (reqWidth > 0 && reqHeight > 0) {
        mPixels->setWindowSize(reqWidth, reqHeight);

        rfb::ScreenSet screens;
        screens.add_screen(rfb::Screen(0, 0, 0, mPixels->width(), mPixels->height(), 0));
        mServer->setScreenLayout(screens);
        return rfb::resultSuccess;
    }

    return rfb::resultInvalid;
}

// cpuconsumer frame listener, called from binder thread
void AndroidDesktop::onFrameAvailable(const BufferItem&) {
    //ALOGV("onFrameAvailable: [%lu] mTimestamp=%ld", item.mFrameNumber, item.mTimestamp);

    notify();
}

void AndroidDesktop::keyEvent(rdr::U32 keysym, __unused_attr rdr::U32 keycode, bool down) {
    mInputDevice->keyEvent(down, keysym);
}

void AndroidDesktop::pointerEvent(const rfb::Point& pos, int buttonMask) {
    if (pos.x < mDisplayRect.left || pos.x > mDisplayRect.right || pos.y < mDisplayRect.top ||
        pos.y > mDisplayRect.bottom) {
        // outside viewport
        return;
    }
    uint32_t spaceX = abs(((float)(mDisplayRect.getWidth() - mPixels->width())) / 2);
    uint32_t x = pos.x - spaceX;
    uint32_t y = pos.y * ((float)(mDisplayRect.getHeight()) / (float)mPixels->height());

    //ALOGV("pointer xlate x1=%d y1=%d x2=%d y2=%d", pos.x, pos.y, x, y);

    mServer->setCursorPos(rfb::Point(x, y));
    mInputDevice->pointerEvent(buttonMask, x, y);
}

// refresh the display dimensions
status_t AndroidDesktop::updateDisplayInfo() {
    const auto displayToken = SurfaceComposerClient::getInternalDisplayToken();
    if (displayToken == nullptr) {
        ALOGE("Failed to get display token\n");
        return -1;
    }

    status_t err = SurfaceComposerClient::getActiveDisplayMode(displayToken, &mDisplayMode);
    if (err != NO_ERROR) {
        ALOGE("Failed to get display configuration\n");
        return err;
    }
    //ALOGV("updateDisplayInfo: [%d:%d]", mDisplayMode.resolution.width, mDisplayMode.resolution.height);

    err = SurfaceComposerClient::getDisplayState(displayToken, &mDisplayState);
    if (err != NO_ERROR) {
        ALOGE("Failed to get current display status");
        return err;
    }

    mPixels->setDisplayInfo(&mDisplayMode, &mDisplayState);

    return NO_ERROR;
}

rfb::ScreenSet AndroidDesktop::computeScreenLayout() {
    rfb::ScreenSet screens;
    screens.add_screen(rfb::Screen(0, 0, 0, mPixels->width(), mPixels->height(), 0));
    return screens;
    mServer->setScreenLayout(screens);
}

void AndroidDesktop::onBufferDimensionsChanged(uint32_t width, uint32_t height) {
    ALOGV("Dimensions changed: old=(%ux%u) new=(%ux%u)", mDisplayRect.getWidth(),
          mDisplayRect.getHeight(), width, height);

    mVirtualDisplay.clear();
    mVirtualDisplay = new VirtualDisplay(&mDisplayMode,  &mDisplayState,
                                         mPixels->width(), mPixels->height(), this);

    mDisplayRect = mVirtualDisplay->getDisplayRect();

    mInputDevice->reconfigure(mDisplayRect.getWidth(), mDisplayRect.getHeight());

    mServer->setPixelBuffer(mPixels.get(), computeScreenLayout());
    mServer->setScreenLayout(computeScreenLayout());
}

void AndroidDesktop::queryConnection(network::Socket* sock, __unused_attr const char* userName) {
    mServer->approveConnection(sock, true, NULL);
}

void AndroidDesktop::terminate() {
    kill(getpid(), SIGTERM);
}
