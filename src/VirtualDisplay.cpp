//
// vncflinger - Copyright (C) 2017 Steve Kondik
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#define LOG_TAG "VNCFlinger:VirtualDisplay"
#include <utils/Log.h>

#include <gui/BufferQueue.h>
#include <gui/CpuConsumer.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/SurfaceComposerClient.h>
#include <input/DisplayViewport.h>
#include "VirtualDisplay.h"

using namespace vncflinger;

VirtualDisplay::VirtualDisplay(ui::DisplayMode* mode, ui::DisplayState* state,
                               uint32_t width, uint32_t height,
                               sp<CpuConsumer::FrameAvailableListener> listener) {
    mWidth = width;
    mHeight = height;

    if (state->orientation == ui::ROTATION_0 || state->orientation == ui::ROTATION_180) {
        mSourceRect = Rect(mode->resolution.width, mode->resolution.height);
    } else {
        mSourceRect = Rect(mode->resolution.height, mode->resolution.width);
    }

    Rect displayRect = getDisplayRect();

    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&mProducer, &consumer);
    mCpuConsumer = new CpuConsumer(consumer, 1);
    mCpuConsumer->setName(String8("vds-to-cpu"));
    mCpuConsumer->setDefaultBufferSize(width, height);
    mProducer->setMaxDequeuedBufferCount(4);
    consumer->setDefaultBufferFormat(PIXEL_FORMAT_RGBX_8888);

    mCpuConsumer->setFrameAvailableListener(listener);

    mDisplayToken = SurfaceComposerClient::createDisplay(String8("VNC-VirtualDisplay"), false /*secure*/);

    SurfaceComposerClient::Transaction t;
    t.setDisplaySurface(mDisplayToken, mProducer);
    t.setDisplayProjection(mDisplayToken, state->orientation, mSourceRect, displayRect);
    t.setDisplayLayerStack(mDisplayToken, ui::DEFAULT_LAYER_STACK);  // default stack
    t.apply();

    ALOGV("Virtual display (%ux%u [viewport=%ux%u] created", width, height, displayRect.getWidth(),
          displayRect.getHeight());
}

VirtualDisplay::~VirtualDisplay() {
    mCpuConsumer.clear();
    mProducer.clear();
    SurfaceComposerClient::destroyDisplay(mDisplayToken);

    ALOGV("Virtual display destroyed");
}

Rect VirtualDisplay::getDisplayRect() {
    uint32_t outWidth, outHeight;
    if (mWidth > (uint32_t)((float)mWidth * aspectRatio())) {
        // limited by narrow width; reduce height
        outWidth = mWidth;
        outHeight = (uint32_t)((float)mWidth * aspectRatio());
    } else {
        // limited by short height; restrict width
        outHeight = mHeight;
        outWidth = (uint32_t)((float)mHeight / aspectRatio());
    }

    // position the desktop in the viewport while preserving
    // the source aspect ratio. we do this in case the client
    // has resized the window and to deal with orientation
    // changes set up by updateDisplayProjection
    uint32_t offX, offY;
    offX = (mWidth - outWidth) / 2;
    offY = (mHeight - outHeight) / 2;
    return Rect(offX, offY, offX + outWidth, offY + outHeight);
}
