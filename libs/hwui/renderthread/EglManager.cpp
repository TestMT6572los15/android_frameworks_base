/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "EglManager.h"

#include <string>

#include "utils/StringUtils.h"
#include <cutils/properties.h>
#include <log/log.h>

#include "Caches.h"
#include "DeviceInfo.h"
#include "Frame.h"
#include "Properties.h"
#include "RenderThread.h"
#include "renderstate/RenderState.h"
#include "Texture.h"

#include <EGL/eglext.h>
#include <GrContextOptions.h>
#include <gl/GrGLInterface.h>

#ifdef HWUI_GLES_WRAP_ENABLED
#include "debug/GlesDriver.h"
#endif

#define GLES_VERSION 2

// Android-specific addition that is used to show when frames began in systrace
EGLAPI void EGLAPIENTRY eglBeginFrame(EGLDisplay dpy, EGLSurface surface);

namespace android {
namespace uirenderer {
namespace renderthread {

#define ERROR_CASE(x) case x: return #x;
static const char* egl_error_str(EGLint error) {
    switch (error) {
        ERROR_CASE(EGL_SUCCESS)
        ERROR_CASE(EGL_NOT_INITIALIZED)
        ERROR_CASE(EGL_BAD_ACCESS)
        ERROR_CASE(EGL_BAD_ALLOC)
        ERROR_CASE(EGL_BAD_ATTRIBUTE)
        ERROR_CASE(EGL_BAD_CONFIG)
        ERROR_CASE(EGL_BAD_CONTEXT)
        ERROR_CASE(EGL_BAD_CURRENT_SURFACE)
        ERROR_CASE(EGL_BAD_DISPLAY)
        ERROR_CASE(EGL_BAD_MATCH)
        ERROR_CASE(EGL_BAD_NATIVE_PIXMAP)
        ERROR_CASE(EGL_BAD_NATIVE_WINDOW)
        ERROR_CASE(EGL_BAD_PARAMETER)
        ERROR_CASE(EGL_BAD_SURFACE)
        ERROR_CASE(EGL_CONTEXT_LOST)
    default:
        return "Unknown error";
    }
}
const char* EglManager::eglErrorString() {
    return egl_error_str(eglGetError());
}

static struct {
    bool bufferAge = false;
    bool setDamage = false;
} EglExtensions;

EglManager::EglManager(RenderThread& thread)
        : mRenderThread(thread)
        , mEglDisplay(EGL_NO_DISPLAY)
        , mEglConfig(nullptr)
        , mEglContext(EGL_NO_CONTEXT)
        , mPBufferSurface(EGL_NO_SURFACE)
        , mCurrentSurface(EGL_NO_SURFACE) {
}

void EglManager::initialize() {
    if (hasEglContext()) return;

    ATRACE_NAME("Creating EGLContext");

    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    LOG_ALWAYS_FATAL_IF(mEglDisplay == EGL_NO_DISPLAY,
            "Failed to get EGL_DEFAULT_DISPLAY! err=%s", eglErrorString());

    EGLint major, minor;
    LOG_ALWAYS_FATAL_IF(eglInitialize(mEglDisplay, &major, &minor) == EGL_FALSE,
            "Failed to initialize display %p! err=%s", mEglDisplay, eglErrorString());

    ALOGI("Initialized EGL, version %d.%d", (int)major, (int)minor);

    initExtensions();

    // Now that extensions are loaded, pick a swap behavior
    if (Properties::enablePartialUpdates) {
        // An Adreno driver bug is causing rendering problems for SkiaGL with
        // buffer age swap behavior (b/31957043).  To temporarily workaround,
        // we will use preserved swap behavior.
        if (Properties::useBufferAge && EglExtensions.bufferAge && !Properties::isSkiaEnabled()) {
            mSwapBehavior = SwapBehavior::BufferAge;
        } else {
            mSwapBehavior = SwapBehavior::Preserved;
        }
    }

    loadConfig();
    createContext();
    createPBufferSurface();
    makeCurrent(mPBufferSurface);
    DeviceInfo::initialize();
    mRenderThread.renderState().onGLContextCreated();

    if (Properties::getRenderPipelineType() == RenderPipelineType::SkiaGL) {
#ifdef HWUI_GLES_WRAP_ENABLED
        debug::GlesDriver* driver = debug::GlesDriver::get();
        sk_sp<const GrGLInterface> glInterface(driver->getSkiaInterface());
#else
        sk_sp<const GrGLInterface> glInterface(GrGLCreateNativeInterface());
#endif
        LOG_ALWAYS_FATAL_IF(!glInterface.get());

        GrContextOptions options;
        options.fGpuPathRenderers &= ~GrContextOptions::GpuPathRenderers::kDistanceField;
        options.fAllowPathMaskCaching = true;
        mRenderThread.setGrContext(GrContext::Create(GrBackend::kOpenGL_GrBackend,
                (GrBackendContext)glInterface.get(), options));
    }
}

void EglManager::initExtensions() {
    auto extensions = StringUtils::split(
            eglQueryString(mEglDisplay, EGL_EXTENSIONS));
    // For our purposes we don't care if EGL_BUFFER_AGE is a result of
    // EGL_EXT_buffer_age or EGL_KHR_partial_update as our usage is covered
    // under EGL_KHR_partial_update and we don't need the expanded scope
    // that EGL_EXT_buffer_age provides.
    EglExtensions.bufferAge = extensions.has("EGL_EXT_buffer_age")
            || extensions.has("EGL_KHR_partial_update");
    EglExtensions.setDamage = extensions.has("EGL_KHR_partial_update");
    LOG_ALWAYS_FATAL_IF(!extensions.has("EGL_KHR_swap_buffers_with_damage"),
            "Missing required extension EGL_KHR_swap_buffers_with_damage");
}

bool EglManager::hasEglContext() {
    return mEglDisplay != EGL_NO_DISPLAY;
}

void EglManager::loadConfig() {
    ALOGD("Swap behavior %d", static_cast<int>(mSwapBehavior));
    EGLint swapBehavior = (mSwapBehavior == SwapBehavior::Preserved)
            ? EGL_SWAP_BEHAVIOR_PRESERVED_BIT : 0;
    EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_CONFIG_CAVEAT, EGL_NONE,
            EGL_STENCIL_SIZE, Stencil::getStencilSize(),
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | swapBehavior,
            EGL_NONE
    };

    EGLint num_configs = 1;
    if (!eglChooseConfig(mEglDisplay, attribs, &mEglConfig, num_configs, &num_configs)
            || num_configs != 1) {
        if (mSwapBehavior == SwapBehavior::Preserved) {
            // Try again without dirty regions enabled
            ALOGW("Failed to choose config with EGL_SWAP_BEHAVIOR_PRESERVED, retrying without...");
            mSwapBehavior = SwapBehavior::Discard;
            loadConfig();
        } else {
            // Failed to get a valid config
            LOG_ALWAYS_FATAL("Failed to choose config, error = %s", eglErrorString());
        }
    }
}

void EglManager::createContext() {
    EGLint attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, GLES_VERSION,
            EGL_NONE
    };
    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, EGL_NO_CONTEXT, attribs);
    LOG_ALWAYS_FATAL_IF(mEglContext == EGL_NO_CONTEXT,
        "Failed to create context, error = %s", eglErrorString());
}

void EglManager::createPBufferSurface() {
    LOG_ALWAYS_FATAL_IF(mEglDisplay == EGL_NO_DISPLAY,
            "usePBufferSurface() called on uninitialized GlobalContext!");

    if (mPBufferSurface == EGL_NO_SURFACE) {
        EGLint attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        mPBufferSurface = eglCreatePbufferSurface(mEglDisplay, mEglConfig, attribs);
    }
}

EGLSurface EglManager::createSurface(EGLNativeWindowType window) {
    initialize();

    EGLint attribs[] = {
#ifdef ANDROID_ENABLE_LINEAR_BLENDING
            EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR,
            EGL_COLORSPACE, EGL_COLORSPACE_sRGB,
#endif
            EGL_NONE
    };

    EGLSurface surface = eglCreateWindowSurface(mEglDisplay, mEglConfig, window, attribs);
    LOG_ALWAYS_FATAL_IF(surface == EGL_NO_SURFACE,
            "Failed to create EGLSurface for window %p, eglErr = %s",
            (void*) window, eglErrorString());

    if (mSwapBehavior != SwapBehavior::Preserved) {
        LOG_ALWAYS_FATAL_IF(eglSurfaceAttrib(mEglDisplay, surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED) == EGL_FALSE,
                            "Failed to set swap behavior to destroyed for window %p, eglErr = %s",
                            (void*) window, eglErrorString());
    }

    return surface;
}

void EglManager::destroySurface(EGLSurface surface) {
    if (isCurrent(surface)) {
        makeCurrent(EGL_NO_SURFACE);
    }
    if (!eglDestroySurface(mEglDisplay, surface)) {
        ALOGW("Failed to destroy surface %p, error=%s", (void*)surface, eglErrorString());
    }
}

void EglManager::destroy() {
    if (mEglDisplay == EGL_NO_DISPLAY) return;

    mRenderThread.setGrContext(nullptr);
    mRenderThread.renderState().onGLContextDestroyed();
    eglDestroyContext(mEglDisplay, mEglContext);
    eglDestroySurface(mEglDisplay, mPBufferSurface);
    eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(mEglDisplay);
    eglReleaseThread();

    mEglDisplay = EGL_NO_DISPLAY;
    mEglContext = EGL_NO_CONTEXT;
    mPBufferSurface = EGL_NO_SURFACE;
    mCurrentSurface = EGL_NO_SURFACE;
}

bool EglManager::makeCurrent(EGLSurface surface, EGLint* errOut) {
    if (isCurrent(surface)) return false;

    if (surface == EGL_NO_SURFACE) {
        // Ensure we always have a valid surface & context
        surface = mPBufferSurface;
    }
    if (!eglMakeCurrent(mEglDisplay, surface, surface, mEglContext)) {
        if (errOut) {
            *errOut = eglGetError();
            ALOGW("Failed to make current on surface %p, error=%s",
                    (void*)surface, egl_error_str(*errOut));
        } else {
            LOG_ALWAYS_FATAL("Failed to make current on surface %p, error=%s",
                    (void*)surface, eglErrorString());
        }
    }
    mCurrentSurface = surface;
    if (Properties::disableVsync) {
        eglSwapInterval(mEglDisplay, 0);
    }
    return true;
}

EGLint EglManager::queryBufferAge(EGLSurface surface) {
    switch (mSwapBehavior) {
    case SwapBehavior::Discard:
        return 0;
    case SwapBehavior::Preserved:
        return 1;
    case SwapBehavior::BufferAge:
        EGLint bufferAge;
        eglQuerySurface(mEglDisplay, surface, EGL_BUFFER_AGE_EXT, &bufferAge);
        return bufferAge;
    }
    return 0;
}

Frame EglManager::beginFrame(EGLSurface surface) {
    LOG_ALWAYS_FATAL_IF(surface == EGL_NO_SURFACE,
            "Tried to beginFrame on EGL_NO_SURFACE!");
    makeCurrent(surface);
    Frame frame;
    frame.mSurface = surface;
    eglQuerySurface(mEglDisplay, surface, EGL_WIDTH, &frame.mWidth);
    eglQuerySurface(mEglDisplay, surface, EGL_HEIGHT, &frame.mHeight);
    frame.mBufferAge = queryBufferAge(surface);
    eglBeginFrame(mEglDisplay, surface);
    return frame;
}

void EglManager::damageFrame(const Frame& frame, const SkRect& dirty) {
#ifdef EGL_KHR_partial_update
    if (EglExtensions.setDamage && mSwapBehavior == SwapBehavior::BufferAge) {
        EGLint rects[4];
        frame.map(dirty, rects);
        if (!eglSetDamageRegionKHR(mEglDisplay, frame.mSurface, rects, 1)) {
            LOG_ALWAYS_FATAL("Failed to set damage region on surface %p, error=%s",
                    (void*)frame.mSurface, eglErrorString());
        }
    }
#endif
}

bool EglManager::damageRequiresSwap() {
    return EglExtensions.setDamage && mSwapBehavior == SwapBehavior::BufferAge;
}

bool EglManager::swapBuffers(const Frame& frame, const SkRect& screenDirty) {

    if (CC_UNLIKELY(Properties::waitForGpuCompletion)) {
        ATRACE_NAME("Finishing GPU work");
        fence();
    }

    eglSwapBuffers(mEglDisplay, frame.mSurface);

    EGLint err = eglGetError();
    if (CC_LIKELY(err == EGL_SUCCESS)) {
        return true;
    }
    if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
        // For some reason our surface was destroyed out from under us
        // This really shouldn't happen, but if it does we can recover easily
        // by just not trying to use the surface anymore
        ALOGW("swapBuffers encountered EGL error %d on %p, halting rendering...",
                err, frame.mSurface);
        return false;
    }
    LOG_ALWAYS_FATAL("Encountered EGL error %d %s during rendering",
            err, egl_error_str(err));
    // Impossible to hit this, but the compiler doesn't know that
    return false;
}

void EglManager::fence() {
    EGLSyncKHR fence = eglCreateSyncKHR(mEglDisplay, EGL_SYNC_FENCE_KHR, NULL);
    eglClientWaitSyncKHR(mEglDisplay, fence,
            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
    eglDestroySyncKHR(mEglDisplay, fence);
}

bool EglManager::setPreserveBuffer(EGLSurface surface, bool preserve) {
    if (mSwapBehavior != SwapBehavior::Preserved) return false;

    bool preserved = eglSurfaceAttrib(mEglDisplay, surface, EGL_SWAP_BEHAVIOR,
            preserve ? EGL_BUFFER_PRESERVED : EGL_BUFFER_DESTROYED);
    if (!preserved) {
        ALOGW("Failed to set EGL_SWAP_BEHAVIOR on surface %p, error=%s",
                (void*) surface, eglErrorString());
        // Maybe it's already set?
        EGLint swapBehavior;
        if (eglQuerySurface(mEglDisplay, surface, EGL_SWAP_BEHAVIOR, &swapBehavior)) {
            preserved = (swapBehavior == EGL_BUFFER_PRESERVED);
        } else {
            ALOGW("Failed to query EGL_SWAP_BEHAVIOR on surface %p, error=%p",
                                (void*) surface, eglErrorString());
        }
    }

    return preserved;
}

} /* namespace renderthread */
} /* namespace uirenderer */
} /* namespace android */
