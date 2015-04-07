/*
**
** Copyright (C) 2008, The Android Open Source Project
** Copyright (C) 2008 HTC Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CameraService"
//#define LOG_NDEBUG 0

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <gui/Surface.h>
#include <hardware/hardware.h>
#include <media/AudioSystem.h>
#include <media/mediaplayer.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/String16.h>

#include "CameraService.h"
#include "CameraClient.h"
#include "Camera2Client.h"

namespace android {

// ----------------------------------------------------------------------------
// Logging support -- this is for debugging only
// Use "adb shell dumpsys media.camera -v 1" to change it.
volatile int32_t gLogLevel = 0;

#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}

// ----------------------------------------------------------------------------

static int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static int getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

// ----------------------------------------------------------------------------

// This is ugly and only safe if we never re-create the CameraService, but
// should be ok for now.
static CameraService *gCameraService;

CameraService::CameraService()
:mSoundRef(0), mModule(0)
{
    ALOGI("CameraService started (pid=%d)", getpid());
    gCameraService = this;
}

void CameraService::onFirstRef()
{
    BnCameraService::onFirstRef();

    if (hw_get_module(CAMERA_HARDWARE_MODULE_ID,
                (const hw_module_t **)&mModule) < 0) {
        ALOGE("Could not load camera HAL module");
        mNumberOfCameras = 0;
    }
    else {
        mNumberOfCameras = mModule->get_number_of_cameras();
        if (mNumberOfCameras > MAX_CAMERAS) {
            ALOGE("Number of cameras(%d) > MAX_CAMERAS(%d).",
                    mNumberOfCameras, MAX_CAMERAS);
            mNumberOfCameras = MAX_CAMERAS;
        }
        for (int i = 0; i < mNumberOfCameras; i++) {
            setCameraFree(i);
        }
    }
}

CameraService::~CameraService() {
    for (int i = 0; i < mNumberOfCameras; i++) {
        if (mBusy[i]) {
            ALOGE("camera %d is still in use in destructor!", i);
        }
    }

    gCameraService = NULL;
}

int32_t CameraService::getNumberOfCameras() {
    return mNumberOfCameras;
}

status_t CameraService::getCameraInfo(int cameraId,
                                      struct CameraInfo* cameraInfo) {
    if (!mModule) {
        return NO_INIT;
    }

    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        return BAD_VALUE;
    }

    struct camera_info info;
    status_t rc = mModule->get_camera_info(cameraId, &info);
    cameraInfo->facing = info.facing;
    cameraInfo->orientation = info.orientation;
    return rc;
}

sp<ICamera> CameraService::connect(
        const sp<ICameraClient>& cameraClient, int cameraId) {
    int callingPid = getCallingPid();

    LOG1("CameraService::connect E (pid %d, id %d)", callingPid, cameraId);

    if (!mModule) {
        ALOGE("Camera HAL module not loaded");
        return NULL;
    }

    sp<Client> client;
    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        ALOGE("CameraService::connect X (pid %d) rejected (invalid cameraId %d).",
            callingPid, cameraId);
        return NULL;
    }

    char value[PROPERTY_VALUE_MAX];
    property_get("sys.secpolicy.camera.disabled", value, "0");
    if (strcmp(value, "1") == 0) {
        // Camera is disabled by DevicePolicyManager.
        ALOGI("Camera is disabled. connect X (pid %d) rejected", callingPid);
        return NULL;
    }

    Mutex::Autolock lock(mServiceLock);
    if (mClient[cameraId] != 0) {
        client = mClient[cameraId].promote();
        if (client != 0) {
            if (cameraClient->asBinder() == client->getCameraClient()->asBinder()) {
                LOG1("CameraService::connect X (pid %d) (the same client)",
                     callingPid);
                return client;
            } else {
                ALOGW("CameraService::connect X (pid %d) rejected (existing client).",
                      callingPid);
                return NULL;
            }
        }
        mClient[cameraId].clear();
    }

    if (mBusy[cameraId]) {
        ALOGW("CameraService::connect X (pid %d) rejected"
                " (camera %d is still busy).", callingPid, cameraId);
        return NULL;
    }

    struct camera_info info;
    if (mModule->get_camera_info(cameraId, &info) != OK) {
        ALOGE("Invalid camera id %d", cameraId);
        return NULL;
    }

    int deviceVersion;
    if (mModule->common.module_api_version == CAMERA_MODULE_API_VERSION_2_0) {
        deviceVersion = info.device_version;
    } else {
        deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
    }

    switch(deviceVersion) {
      case CAMERA_DEVICE_API_VERSION_1_0:
        client = new CameraClient(this, cameraClient, cameraId,
                info.facing, callingPid);
        break;
      case CAMERA_DEVICE_API_VERSION_2_0:
        client = new Camera2Client(this, cameraClient, cameraId,
                info.facing, callingPid);
        break;
      default:
        ALOGE("Unknown camera device HAL version: %d", deviceVersion);
        return NULL;
    }

    if (client->initialize(mModule) != OK) {
        return NULL;
    }

    mClient[cameraId] = client;
    LOG1("CameraService::connect X (id %d)", cameraId);
    return client;
}

void CameraService::removeClient(const sp<ICameraClient>& cameraClient) {
    int callingPid = getCallingPid();
    LOG1("CameraService::removeClient E (pid %d)", callingPid);

    for (int i = 0; i < mNumberOfCameras; i++) {
        // Declare this before the lock to make absolutely sure the
        // destructor won't be called with the lock held.
        sp<Client> client;

        Mutex::Autolock lock(mServiceLock);

        // This happens when we have already disconnected (or this is
        // just another unused camera).
        if (mClient[i] == 0) continue;

        // Promote mClient. It can fail if we are called from this path:
        // Client::~Client() -> disconnect() -> removeClient().
        client = mClient[i].promote();

        if (client == 0) {
            mClient[i].clear();
            continue;
        }

        if (cameraClient->asBinder() == client->getCameraClient()->asBinder()) {
            // Found our camera, clear and leave.
            LOG1("removeClient: clear camera %d", i);
            mClient[i].clear();
            break;
        }
    }

    LOG1("CameraService::removeClient X (pid %d)", callingPid);
}

CameraService::Client* CameraService::getClientByIdUnsafe(int cameraId) {
    if (cameraId < 0 || cameraId >= mNumberOfCameras) return NULL;
    return mClient[cameraId].unsafe_get();
}

Mutex* CameraService::getClientLockById(int cameraId) {
    if (cameraId < 0 || cameraId >= mNumberOfCameras) return NULL;
    return &mClientLock[cameraId];
}

status_t CameraService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    // Permission checks
    switch (code) {
        case BnCameraService::CONNECT:
            const int pid = getCallingPid();
            const int self_pid = getpid();
            if (pid != self_pid) {
                // we're called from a different process, do the real check
                if (!checkCallingPermission(
                        String16("android.permission.CAMERA"))) {
                    const int uid = getCallingUid();
                    ALOGE("Permission Denial: "
                         "can't use the camera pid=%d, uid=%d", pid, uid);
                    return PERMISSION_DENIED;
                }
            }
            break;
    }

    return BnCameraService::onTransact(code, data, reply, flags);
}

// The reason we need this busy bit is a new CameraService::connect() request
// may come in while the previous Client's destructor has not been run or is
// still running. If the last strong reference of the previous Client is gone
// but the destructor has not been finished, we should not allow the new Client
// to be created because we need to wait for the previous Client to tear down
// the hardware first.
void CameraService::setCameraBusy(int cameraId) {
    android_atomic_write(1, &mBusy[cameraId]);
}

void CameraService::setCameraFree(int cameraId) {
    android_atomic_write(0, &mBusy[cameraId]);
}

// We share the media players for shutter and recording sound for all clients.
// A reference count is kept to determine when we will actually release the
// media players.

MediaPlayer* CameraService::newMediaPlayer(const char *file) {
    MediaPlayer* mp = new MediaPlayer();
    if (mp->setDataSource(file, NULL) == NO_ERROR) {
        mp->setAudioStreamType(AUDIO_STREAM_ENFORCED_AUDIBLE);
        mp->prepare();
    } else {
        ALOGE("Failed to load CameraService sounds: %s", file);
        return NULL;
    }
    return mp;
}

void CameraService::loadSound() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::loadSound ref=%d", mSoundRef);
    if (mSoundRef++) return;

    mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/system/media/audio/ui/camera_click.ogg");
    mSoundPlayer[SOUND_RECORDING] = newMediaPlayer("/system/media/audio/ui/VideoRecord.ogg");
}

void CameraService::releaseSound() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::releaseSound ref=%d", mSoundRef);
    if (--mSoundRef) return;

    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (mSoundPlayer[i] != 0) {
            mSoundPlayer[i]->disconnect();
            mSoundPlayer[i].clear();
        }
    }
}

void CameraService::playSound(sound_kind kind) {
    LOG1("playSound(%d)", kind);
    Mutex::Autolock lock(mSoundLock);
    sp<MediaPlayer> player = mSoundPlayer[kind];
    if (player != 0) {
        player->seekTo(0);
        player->start();
    }
}

// ----------------------------------------------------------------------------

CameraService::Client::Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        int cameraId, int cameraFacing, int clientPid) {
    int callingPid = getCallingPid();
    LOG1("Client::Client E (pid %d, id %d)", callingPid, cameraId);

    mCameraService = cameraService;
    mCameraClient = cameraClient;
    mCameraId = cameraId;
    mCameraFacing = cameraFacing;
    mClientPid = clientPid;
    mDestructionStarted = false;

    cameraService->setCameraBusy(cameraId);
    cameraService->loadSound();
    LOG1("Client::Client X (pid %d, id %d)", callingPid, cameraId);
}

// tear down the client
CameraService::Client::~Client() {
    mCameraService->releaseSound();
}

// ----------------------------------------------------------------------------

Mutex* CameraService::Client::getClientLockFromCookie(void* user) {
    return gCameraService->getClientLockById((int) user);
}

// Provide client pointer for callbacks. Client lock returned from getClientLockFromCookie should
// be acquired for this to be safe
CameraService::Client* CameraService::Client::getClientFromCookie(void* user) {
    Client* client = gCameraService->getClientByIdUnsafe((int) user);

    // This could happen if the Client is in the process of shutting down (the
    // last strong reference is gone, but the destructor hasn't finished
    // stopping the hardware).
    if (client == NULL) return NULL;

    // destruction already started, so should not be accessed
    if (client->mDestructionStarted) return NULL;

    return client;
}

void CameraService::Client::disconnect() {
    mCameraService->removeClient(mCameraClient);
    mCameraService->setCameraFree(mCameraId);
}

// ----------------------------------------------------------------------------

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

status_t CameraService::dump(int fd, const Vector<String16>& args) {
    String8 result;
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result.appendFormat("Permission Denial: "
                "can't dump CameraService from pid=%d, uid=%d\n",
                getCallingPid(),
                getCallingUid());
        write(fd, result.string(), result.size());
    } else {
        bool locked = tryLock(mServiceLock);
        // failed to lock - CameraService is probably deadlocked
        if (!locked) {
            result.append("CameraService may be deadlocked\n");
        }

        bool hasClient = false;
        for (int i = 0; i < mNumberOfCameras; i++) {
            sp<Client> client = mClient[i].promote();
            if (client == 0) continue;
            hasClient = true;
            client->dump(fd, args);
        }
        if (!hasClient) {
            result.append("No camera clients yet.\n");
        }

        if (locked) mServiceLock.unlock();

        // change logging level
        int n = args.size();
        for (int i = 0; i + 1 < n; i++) {
            String16 verboseOption("-v");
            if (args[i] == verboseOption) {
                String8 levelStr(args[i+1]);
                int level = atoi(levelStr.string());
                result.appendFormat("Setting log level to %d.\n", level);
                setLogLevel(level);
            }
        }
        write(fd, result.string(), result.size());
    }
    return NO_ERROR;
}

}; // namespace android
