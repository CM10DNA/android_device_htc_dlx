/* AudioSessionOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 **
 ** Not a Contribution, Apache license notifications and license are
 ** retained for attribution purposes only.
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

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioSessionOutALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include <linux/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <linux/unistd.h>

#include "AudioHardwareALSA.h"

namespace sys_write {
    ssize_t lib_write(int fd, const void *buf, size_t count) {
        return write(fd, buf, count);
    }
};
namespace android_audio_legacy
{
#define LPA_MODE 0
#define TUNNEL_MODE 1
#define NUM_FDS 2
#define KILL_EVENT_THREAD 1
#define BUFFER_COUNT 4
#define LPA_BUFFER_SIZE 256*1024
#define TUNNEL_BUFFER_SIZE 240*1024
#define TUNNEL_METADATA_SIZE 64
#define MONO_CHANNEL_MODE 1
// ----------------------------------------------------------------------------

AudioSessionOutALSA::AudioSessionOutALSA(AudioHardwareALSA *parent,
                                         uint32_t   devices,
                                         int        format,
                                         uint32_t   channels,
                                         uint32_t   samplingRate,
                                         int        type,
                                         status_t   *status)
{

    alsa_handle_t alsa_handle;
    char *use_case;
    bool bIsUseCaseSet = false;

    Mutex::Autolock autoLock(mLock);
    // Default initilization
    mParent             = parent;
    mAlsaDevice         = mParent->mALSADevice;
    mUcMgr              = mParent->mUcMgr;
    mFormat             = format;
    mSampleRate         = samplingRate;
    mChannels           = channels;


    mBufferSize         = 0;
    *status             = BAD_VALUE;

    mPaused             = false;
    mSeeking            = false;
    mReachedEOS         = false;
    mSkipWrite          = false;

    mAlsaHandle         = NULL;
    mUseCase            = AudioHardwareALSA::USECASE_NONE;

    mInputBufferSize    = type ? TUNNEL_BUFFER_SIZE : LPA_BUFFER_SIZE;
    mInputBufferCount   = BUFFER_COUNT;
    mEfd = -1;
    mEosEventReceived   = false;
    mEventThread        = NULL;
    mEventThreadAlive   = false;
    mKillEventThread    = false;
    mObserver           = NULL;
    mOutputMetadataLength = 0;
    mSkipEOS            = false;
    mTunnelMode         = false;

    if(devices == 0) {
        ALOGE("No output device specified");
        return;
    }
    if((format == AUDIO_FORMAT_PCM_16_BIT) && (channels == 0 || channels > 6)) {
        ALOGE("Invalid number of channels %d", channels);
        return;
    }

    if(mParent->isExtOutDevice(devices)) {
        ALOGE("Set Capture from proxy true");
        mParent->mRouteAudioToExtOut = true;
        if(mParent->mExtOutStream == NULL) {
            mParent->switchExtOut(devices);
        }
    }

    //open device based on the type (LPA or Tunnel) and devices
    //TODO: Check format type for linear vs non-linear to determine LPA/Tunnel
    *status = openAudioSessionDevice(type, devices);

    if (*status != NO_ERROR) {
        ALOGE("Failed to open LPA/Tunnel Session");
        return;
    }
    //Creates the event thread to poll events from LPA/Compress Driver
    createEventThread();

    mUseCase = mParent->useCaseStringToEnum(mAlsaHandle->useCase);
    ALOGV("mParent->mRouteAudioToExtOut = %d", mParent->mRouteAudioToExtOut);
    if (mParent->mRouteAudioToExtOut) {
        status_t err = NO_ERROR;
        err = mParent->startPlaybackOnExtOut_l(mUseCase);
        *status = err;
    }

    *status = NO_ERROR;
}

AudioSessionOutALSA::~AudioSessionOutALSA()
{
    ALOGD("~AudioSessionOutALSA");
    mSkipWrite = true;

    mWriteCv.signal();
    // trying to acquire mDecoderLock, make sure that, the waiting decoder thread
    // receives the signal before the conditional variable "mWriteCv" is
    // destroyed in ~AudioSessionOut(). Decoder thread acquires this lock
    // before it waits for the signal.
    Mutex::Autolock autoDecoderLock(mDecoderLock);
    //TODO: This might need to be Locked using Parent lock
    reset();
    if (mParent->mRouteAudioToExtOut) {
         status_t err = mParent->stopPlaybackOnExtOut_l(mUseCase);
         if(err){
             ALOGE("stopPlaybackOnExtOut_l return err  %d", err);
         }
    }
}

status_t AudioSessionOutALSA::setVolume(float left, float right)
{
    Mutex::Autolock autoLock(mLock);
    float volume;
    status_t status = NO_ERROR;

    volume = (left + right) / 2;
    if (volume < 0.0) {
        ALOGW("AudioSessionOutALSA::setVolume(%f) under 0.0, assuming 0.0\n", volume);
        volume = 0.0;
    } else if (volume > 1.0) {
        ALOGW("AudioSessionOutALSA::setVolume(%f) over 1.0, assuming 1.0\n", volume);
        volume = 1.0;
    }
    mStreamVol = lrint((volume * 0x2000)+0.5);

    ALOGV("Setting stream volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
    if(mAlsaHandle) {
        if(!strcmp(mAlsaHandle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER) ||
           !strcmp(mAlsaHandle->useCase, SND_USE_CASE_MOD_PLAY_LPA)) {
            ALOGD("setLpaVolume(%u)\n", mStreamVol);
            ALOGD("Setting LPA volume to %d (available range is 0 to 100)\n", mStreamVol);
            mAlsaHandle->module->setLpaVolume(mStreamVol);
            return status;
        }
        else if(!strcmp(mAlsaHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL) ||
                !strcmp(mAlsaHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL)) {
            ALOGD("setCompressedVolume(%u)\n", mStreamVol);
            ALOGD("Setting Compressed volume to %d (available range is 0 to 100)\n", mStreamVol);
            mAlsaHandle->module->setCompressedVolume(mStreamVol);
            return status;
        }
    }
    return INVALID_OPERATION;
}


status_t AudioSessionOutALSA::openAudioSessionDevice(int type, int devices)
{
    char* use_case;
    status_t status = NO_ERROR;
    //1.) Based on the current device and session type (LPA/Tunnel), open a device
    //    with verb or modifier
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if (type == LPA_MODE) {
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                            strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            status = openDevice(SND_USE_CASE_VERB_HIFI_LOW_POWER, true, devices);
        } else {
            status = openDevice(SND_USE_CASE_MOD_PLAY_LPA, false, devices);
        }
    } else if (type == TUNNEL_MODE) {
        if ((use_case == NULL) || (!strncmp(use_case, SND_USE_CASE_VERB_INACTIVE,
                                            strlen(SND_USE_CASE_VERB_INACTIVE)))) {
            status = openDevice(SND_USE_CASE_VERB_HIFI_TUNNEL, true, devices);
        } else {
            status = openDevice(SND_USE_CASE_MOD_PLAY_TUNNEL, false, devices);
        }
        mOutputMetadataLength = sizeof(output_metadata_handle_t);
        ALOGD("openAudioSessionDevice - mOutputMetadataLength = %d", mOutputMetadataLength);
        mTunnelMode = true;
    }

    if(use_case) {
        free(use_case);
        use_case = NULL;
    }
    if(status != NO_ERROR) {
        return status;
    }

    //2.) Get the device handle
    ALSAHandleList::iterator it = mParent->mDeviceList.end();
    it--;

    mAlsaHandle = &(*it);
    ALOGV("mAlsaHandle %p, mAlsaHandle->useCase %s",mAlsaHandle, mAlsaHandle->useCase);

    //3.) mmap the buffers for playback
    status_t err = mmap_buffer(mAlsaHandle->handle);
    if(err) {
        ALOGE("MMAP buffer failed - playback err = %d", err);
        return err;
    }
    ALOGV("buffer pointer %p ", mAlsaHandle->handle->addr);

    //4.) prepare the driver for playback and allocate the buffers
    status = pcm_prepare(mAlsaHandle->handle);
    if (status) {
        ALOGE("PCM Prepare failed - playback err = %d", err);
        return status;
    }
    bufferAlloc(mAlsaHandle);
    mBufferSize = mAlsaHandle->periodSize;
    return NO_ERROR;
}

ssize_t AudioSessionOutALSA::write(const void *buffer, size_t bytes)
{
    Mutex::Autolock autoLock(mLock);
    int err;

    ALOGV("write Empty Queue size() = %d, Filled Queue size() = %d "
          "mReached EOS %d, mEosEventReceived %d bytes %d",
          mEmptyQueue.size(),mFilledQueue.size(), mReachedEOS, mEosEventReceived, bytes);

    mEosEventReceived = false;
    mReachedEOS = false;

    if (!bytes) {
        mReachedEOS = true;
    }

    //1.) Dequeue the buffer from empty buffer queue. Copy the data to be
    //    written into the buffer. Then Enqueue the buffer to the filled
    //    buffer queue

    if (mSkipWrite) {
        LOG_ALWAYS_FATAL_IF((mEmptyQueue.size() != BUFFER_COUNT),
                            "::write, mSkipwrite is true but empty queue isnt full");
        ALOGD("reset mSkipWrite in write");
        mSkipWrite = false;
        ALOGD("mSkipWrite is false now write bytes %d", bytes);
        ALOGD("skipping buffer in write");
        return 0;
    }

    ALOGV("not skipping buffer in write since mSkipWrite = %d, "
              "mEmptyQueuesize %d ", mSkipWrite, mEmptyQueue.size());

    List<BuffersAllocated>::iterator it = mEmptyQueue.begin();
    BuffersAllocated buf = *it;

    mEmptyQueue.erase(it);

    if (mTunnelMode) {
        updateMetaData(bytes);

        memcpy(buf.memBuf, &mOutputMetadataTunnel, mOutputMetadataLength);
        ALOGD("Copy Metadata = %d, bytes = %d", mOutputMetadataLength, bytes);

        if (bytes == 0) {
            buf.bytesToWrite = 0;
            err = pcm_write(mAlsaHandle->handle, buf.memBuf, mAlsaHandle->handle->period_size);

            //bad part is !err does not guarantee pcm_write succeeded!
            if (!err) { //mReachedEOS is already set
                /*
                 * This workaround is needed to ensure EOS from the event thread
                 * is posted when the first (only) buffer given to the driver
                 * is a zero length buffer. Note that the compressed driver
                 * does not interrupt the timer fd if the EOS buffer was queued
                 * after a buffer with valid data (full or partial). So we
                 * only need to do this in this special case.
                 */
                if (mFilledQueue.empty()) {
                    mFilledQueue.push_back(buf);
                }
            }

            return err;
        }
    }
    ALOGV("PCM write before memcpy start");
    memcpy((buf.memBuf + mOutputMetadataLength), buffer, bytes);

    //memset the remaining to 0, only for non-tunnel
    if (!mTunnelMode) {
        // zero out the remaining for silence
        size_t freebytes = (mAlsaHandle->handle->period_size
                                - (mOutputMetadataLength + bytes));
        if ((ssize_t)freebytes > 0) { //this is a partial buffer
            memset((buf.memBuf + mOutputMetadataLength + bytes), 0, freebytes);
        }
    }

    buf.bytesToWrite = bytes;

    //2.) Write the buffer to the Driver
    ALOGV("PCM write start");
    err = pcm_write(mAlsaHandle->handle, buf.memBuf, mAlsaHandle->handle->period_size);
    ALOGV("PCM write complete");
    if (bytes < (mAlsaHandle->handle->period_size - mOutputMetadataLength)) {
        ALOGV("Last buffer case %d", mAlsaHandle->handle->start);
        if(!mAlsaHandle->handle->start) {
            if ( ioctl(mAlsaHandle->handle->fd, SNDRV_PCM_IOCTL_START) < 0 ) {
                ALOGE("Audio Start failed");
            } else {
                mAlsaHandle->handle->start = 1;
            }
        }

        if (!mTunnelMode) mReachedEOS = true;
    }

    mFilledQueue.push_back(buf);
    return err;
}

void AudioSessionOutALSA::bufferAlloc(alsa_handle_t *handle) {
    void  *mem_buf = NULL;
    int i = 0;

    int32_t nSize = mAlsaHandle->handle->period_size;
    ALOGV("number of input buffers = %d", mInputBufferCount);
    ALOGV("memBufferAlloc calling with required size %d", nSize);
    for (i = 0; i < mInputBufferCount; i++) {
        mem_buf = (int32_t *)mAlsaHandle->handle->addr + (nSize * i/sizeof(int));
        ALOGV("Buffer pointer %p ", mem_buf);
        BuffersAllocated buf(mem_buf, nSize);
        memset(buf.memBuf, 0x0, nSize);
        mEmptyQueue.push_back(buf);
        mBufPool.push_back(buf);
        ALOGV("The MEM that is allocated - buffer is %x",\
            (unsigned int)mem_buf);
    }
}

void AudioSessionOutALSA::bufferDeAlloc() {
    while (!mBufPool.empty()) {
        List<BuffersAllocated>::iterator it = mBufPool.begin();
        ALOGV("Removing input buffer from Buffer Pool ");
        mBufPool.erase(it);
   }
}

void AudioSessionOutALSA::requestAndWaitForEventThreadExit() {
    if (!mEventThreadAlive)
        return;
    mKillEventThread = true;
    if(mEfd != -1) {
        ALOGE("Writing to mEfd %d",mEfd);
        uint64_t writeValue = KILL_EVENT_THREAD;
        sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));
    }
    pthread_join(mEventThread,NULL);
    ALOGV("event thread killed");
}

void * AudioSessionOutALSA::eventThreadWrapper(void *me) {
    static_cast<AudioSessionOutALSA *>(me)->eventThreadEntry();
    return NULL;
}

void  AudioSessionOutALSA::eventThreadEntry() {
    //1.) Initialize the variables required for polling events
    int rc = 0;
    int err_poll = 0;
    int avail = 0;
    int i = 0;
    struct pollfd pfd[NUM_FDS];
    int timeout = -1;

    //2.) Set the priority for the event thread
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"HAL Audio EventThread", 0, 0, 0);

    //3.) Allocate two FDs for polling.
    //    1st FD: Polling on the Driver's timer_fd. This is used for getting write done
    //            events from the driver
    //    2nd FD: Polling on a local fd so we can interrup the event thread locally
    //            when playback is stopped from Apps
    //    The event thread will when a write is performed on one of these FDs
    ALOGV("Allocating poll fd");
    if(!mKillEventThread) {
        pfd[0].fd = mAlsaHandle->handle->timer_fd;
        pfd[0].events = (POLLIN | POLLERR | POLLNVAL);
        mEfd = eventfd(0,0);
        pfd[1].fd = mEfd;
        pfd[1].events = (POLLIN | POLLERR | POLLNVAL);
    }

    //4.) Start a poll for write done events from driver.
    while(!mKillEventThread && ((err_poll = poll(pfd, NUM_FDS, timeout)) >=0)) {
        ALOGV("pfd[0].revents =%d ", pfd[0].revents);
        ALOGV("pfd[1].revents =%d ", pfd[1].revents);
        // Handle Poll errors
        if (err_poll == EINTR)
            ALOGE("Timer is intrrupted");
        if ((pfd[1].revents & POLLERR) || (pfd[1].revents & POLLNVAL)) {
            pfd[1].revents = 0;
            ALOGE("POLLERR or INVALID POLL");
        }

        //POLLIN event on 2nd FD. Kill from event thread
        if (pfd[1].revents & POLLIN) {
            uint64_t u;
            read(mEfd, &u, sizeof(uint64_t));
            ALOGV("POLLIN event occured on the event fd, value written to %llu",
                 (unsigned long long)u);
            pfd[1].revents = 0;
            if (u == KILL_EVENT_THREAD) {
                continue;
            }
        }

        //Poll error on Driver's timer fd
        if((pfd[0].revents & POLLERR) || (pfd[0].revents & POLLNVAL)) {
            pfd[0].revents = 0;
            continue;
        }

        //Pollin event on Driver's timer fd
        if (pfd[0].revents & POLLIN && !mKillEventThread) {
            struct snd_timer_tread rbuf[4];
            ALOGV("mAlsaHandle->handle = %p", mAlsaHandle->handle);
            if( !mAlsaHandle->handle ) {
                ALOGD(" mAlsaHandle->handle is NULL, breaking from while loop in eventthread");
                pfd[0].revents = 0;
                break;
            }
            read(mAlsaHandle->handle->timer_fd, rbuf, sizeof(struct snd_timer_tread) * 4 );
            pfd[0].revents = 0;
            ALOGV("After an event occurs");

            {
                Mutex::Autolock _l(mLock);
                if (mFilledQueue.empty()) {
                    ALOGV("Filled queue is empty"); //only time this would be valid is after a flush?
                    continue;
                }
                // Transfer a buffer that was consumed by the driver from filled queue
                // to empty queue

                BuffersAllocated buf = *(mFilledQueue.begin());
                mFilledQueue.erase(mFilledQueue.begin());
                ALOGV("mFilledQueue %d", mFilledQueue.size());

                mEmptyQueue.push_back(buf);
                mWriteCv.signal();
                ALOGV("Reset mSkipwrite in eventthread entry");
                mSkipWrite = false;

                //Post EOS in case the filled queue is empty and EOS is reached.
                if (mFilledQueue.empty() && mReachedEOS) {
                    drainAndPostEOS_l();
                }
            }
        }
    }

    //5.) Close mEfd that was created
    mEventThreadAlive = false;
    if (mEfd != -1) {
        close(mEfd);
        mEfd = -1;
    }
    ALOGV("Event Thread is dying.");
    return;

}

void AudioSessionOutALSA::createEventThread() {
    ALOGV("Creating Event Thread");
    mKillEventThread = false;
    mEventThreadAlive = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&mEventThread, &attr, eventThreadWrapper, this);
    ALOGV("Event Thread created");
}

status_t AudioSessionOutALSA::start()
{
    Mutex::Autolock autoLock(mLock);
    ALOGV("AudioSessionOutALSA start()");
    //we should not reset EOS here, since EOS could have been
    //marked in write, the only place to clear EOS should
    //be flush
    //mEosEventReceived = false;
    //mReachedEOS = false;
    //mSkipEOS = false;
    if (mPaused) {
        ALOGV("AudioSessionOutALSA ::start mPaused true");
        status_t err = NO_ERROR;
        if (mSeeking) {
            ALOGV("AudioSessionOutALSA ::start before drain");
            drain();
            ALOGV("AudioSessionOutALSA ::start after drain");
            mSeeking = false;
        } else {
            ALOGV("AudioSessionOutALSA ::start before resume");
            if (resume_l() == UNKNOWN_ERROR) {
                ALOGV("AudioSessionOutALSA ::start after resume error");
                return UNKNOWN_ERROR;
            }
        }
        mPaused = false;
    } else if (!mAlsaHandle->handle->start) {
        //Signal the driver to start rendering data
        ALOGV("AudioSessionOutALSA ::start calling _ioctl_start");
        if (ioctl(mAlsaHandle->handle->fd, SNDRV_PCM_IOCTL_START)) {
            ALOGE("start:SNDRV_PCM_IOCTL_START failed\n");
            return UNKNOWN_ERROR;
        }
        mAlsaHandle->handle->start = 1;
    }
    ALOGV("AudioSessionOutALSA ::start done");
    return NO_ERROR;
}

status_t AudioSessionOutALSA::pause()
{
    Mutex::Autolock autoLock(mLock);
    status_t err = NO_ERROR;
    ALOGD("Pausing the driver");
    //Signal the driver to pause rendering data
    if (pause_l() == UNKNOWN_ERROR) {
        return UNKNOWN_ERROR;
    }
    mPaused = true;

    if(err) {
        ALOGE("pause returned error");
        return err;
    }
    return err;

}

status_t AudioSessionOutALSA::pause_l()
{
    if (!mPaused) {
        if (ioctl(mAlsaHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            ALOGE("PAUSE failed on use case %s", mAlsaHandle->useCase);
            return UNKNOWN_ERROR;
        }
    }
    return NO_ERROR;
}

status_t AudioSessionOutALSA::resume_l()
{
    status_t err = NO_ERROR;
    if (mPaused) {
        if (ioctl(mAlsaHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0) {
            ALOGE("Resume failed on use case %s", mAlsaHandle->useCase);
            return UNKNOWN_ERROR;
        }
    }
    return NO_ERROR;
}

status_t AudioSessionOutALSA::drain()
{
    mAlsaHandle->handle->start = 0;
    int err = pcm_prepare(mAlsaHandle->handle);
    if(err != OK) {
        ALOGE("pcm_prepare -seek = %d",err);
        //Posting EOS
        if (mObserver)
            mObserver->postEOS(0);
        return UNKNOWN_ERROR;
    }

    ALOGV("drain Empty Queue size() = %d, Filled Queue size() = %d ",
         mEmptyQueue.size(), mFilledQueue.size());

    mAlsaHandle->handle->sync_ptr->flags =
        SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync_ptr(mAlsaHandle->handle);
    ALOGV("appl_ptr=%d",(int)mAlsaHandle->handle->sync_ptr->c.control.appl_ptr);
    return NO_ERROR;
}

status_t AudioSessionOutALSA::flush()
{
    Mutex::Autolock autoLock(mLock);
    ALOGV("AudioSessionOutALSA flush");
    int err;
    {
        // 1.) Clear the Empty and Filled buffer queue
        mEmptyQueue.clear();
        mFilledQueue.clear();

        // 2.) Add all the available buffers to Request Queue (Maintain order)
        List<BuffersAllocated>::iterator it = mBufPool.begin();
        for (; it!=mBufPool.end(); ++it) {
            memset(it->memBuf, 0x0, (*it).memBufsize);
            mEmptyQueue.push_back(*it);
        }
    }

    ALOGV("Transferred all the buffers from Filled queue to "
          "Empty queue to handle seek paused %d, skipwrite %d", mPaused, mSkipWrite);
    ALOGV("Set mReachedEOS to false and mEosEventReceived to false");
    mReachedEOS = false;
    mEosEventReceived = false;
    mSkipEOS = false;
    // 3.) If its in start state,
    //          Pause and flush the driver and Resume it again
    //    If its in paused state,
    //          Set the seek flag, Resume will take care of flushing the
    //          driver
    if (!mPaused) {
      ALOGV("AudioSessionOutALSA flush going to call Pause");
      if ((err = ioctl(mAlsaHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1)) < 0) {
        ALOGE("Audio Pause failed - continuing");
        //return UNKNOWN_ERROR;
      }
    } else {
        mSeeking = true;
    }

    //drain has to be called every time irrespective of whether its paused or not
    if ((err = drain()) != OK) {
        ALOGE("pcm_prepare failed - continuing");
       //return err;
    }

    //4.) Skip the current write from the decoder and signal to the Write get
    //   the next set of data from the decoder
    mSkipWrite = true;
    ALOGV("signalling from flush mSkipWrite %d", mSkipWrite);
    mWriteCv.signal();

    ALOGV("AudioSessionOutALSA::flush completed");
    return NO_ERROR;
}



status_t AudioSessionOutALSA::stop()
{
    Mutex::Autolock autoLock(mLock);
    ALOGV("AudioSessionOutALSA- stop");
    // close all the existing PCM devices
    mSkipWrite = true;
    mWriteCv.signal();

    if (mParent->mRouteAudioToExtOut) {
        status_t err = mParent->suspendPlaybackOnExtOut(mUseCase);
        if(err) {
            ALOGE("stop-suspendPlaybackOnExtOut- return err = %d", err);
            return err;
        }
    }

    ALOGV("stop -");

    return NO_ERROR;
}

status_t AudioSessionOutALSA::standby()
{
    Mutex::Autolock autoLock(mParent->mLock);
    // At this point, all the buffers with the driver should be
    // flushed.
    status_t err = NO_ERROR;
    mLock.lock();
    if( !mAlsaHandle->handle->start) {
        ALOGV("drain from destructor which will flush the buffers");
        drain();
    }
    mLock.unlock();
    mAlsaHandle->module->standby(mAlsaHandle);
    if (mParent->mRouteAudioToExtOut) {
         ALOGD("Standby - stopPlaybackOnExtOut_l - mUseCase = %d",mUseCase);
         err = mParent->stopPlaybackOnExtOut_l(mUseCase);
         if(err){
             ALOGE("stopPlaybackOnExtOut_l return err  %d", err);
         }
    }
    mPaused = false;
    return err;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioSessionOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mAlsaHandle->latency);
}

status_t AudioSessionOutALSA::setObserver(void *observer)
{
    ALOGV("Registering the callback \n");
    mObserver = reinterpret_cast<AudioEventObserver *>(observer);
    return NO_ERROR;
}

status_t AudioSessionOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioSessionOutALSA::getNextWriteTimestamp(int64_t *timestamp)
{
    struct snd_compr_tstamp tstamp;
    tstamp.timestamp = -1;
    if (ioctl(mAlsaHandle->handle->fd, SNDRV_COMPRESS_TSTAMP, &tstamp)){
        ALOGE("Failed SNDRV_COMPRESS_TSTAMP\n");
        return UNKNOWN_ERROR;
    } else {
        ALOGV("Timestamp returned = %lld\n", tstamp.timestamp);
        *timestamp = tstamp.timestamp;
        return NO_ERROR;
    }
    return NO_ERROR;
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioSessionOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    Mutex::Autolock autoLock(mLock);
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

status_t AudioSessionOutALSA::getBufferInfo(buf_info **buf) {
    if (!mAlsaHandle) {
        return NO_ERROR;
    }
    buf_info *tempbuf = (buf_info *)malloc(sizeof(buf_info) + mInputBufferCount*sizeof(int *));
    ALOGV("Get buffer info");
    tempbuf->bufsize = mAlsaHandle->handle->period_size;
    tempbuf->nBufs = mInputBufferCount;
    tempbuf->buffers = (int **)((char*)tempbuf + sizeof(buf_info));
    List<BuffersAllocated>::iterator it = mBufPool.begin();
    for (int i = 0; i < mInputBufferCount; i++) {
        tempbuf->buffers[i] = (int *)it->memBuf;
        it++;
    }
    *buf = tempbuf;
    return NO_ERROR;
}

status_t AudioSessionOutALSA::isBufferAvailable(int *isAvail) {

    Mutex::Autolock autoLock(mLock);
    // this lock is required to synchronize between decoder thread and control thread.
    // if this thread is waiting for a signal on the conditional ariable "mWriteCv"
    // and the ~AudioSessionOut() signals but the mWriteCv is destroyed, before the
    // signal reaches the waiting thread, it can lead to an indefinite wait resulting
    // in deadlock.
    ALOGV("acquiring mDecoderLock in isBufferAvailable()");
    Mutex::Autolock autoDecoderLock(mDecoderLock);
    ALOGV("isBufferAvailable Empty Queue size() = %d, Filled Queue size() = %d ",
          mEmptyQueue.size(),mFilledQueue.size());
    *isAvail = false;

    /*
     * Only time the below condition is true is when isBufferAvailable is called
     * immediately after a flush
     */
    if (mSkipWrite) {
        LOG_ALWAYS_FATAL_IF((mEmptyQueue.size() != BUFFER_COUNT),
                            "::isBufferAvailable, mSkipwrite is true but empty queue isnt full");
        mSkipWrite = false;
    }
    // 1.) Wait till a empty buffer is available in the Empty buffer queue
    while (mEmptyQueue.empty()) {
        ALOGV("Write: waiting on mWriteCv");
        mWriteCv.wait(mLock);
        if (mSkipWrite) {
            ALOGV("Write: Flushing the previous write buffer");
            mSkipWrite = false;
            return NO_ERROR;
        }
        ALOGV("isBufferAvailable: received a signal to wake up");
    }

    *isAvail = true;
    return NO_ERROR;
}

status_t AudioSessionOutALSA::openDevice(char *useCase, bool bIsUseCase, int devices)
{
    alsa_handle_t alsa_handle;
    status_t status = NO_ERROR;
    ALOGV("openDevice: E usecase %s", useCase);
    alsa_handle.module      = mAlsaDevice;
    alsa_handle.bufferSize  = mInputBufferSize;
    alsa_handle.devices     = devices;
    alsa_handle.handle      = 0;
    alsa_handle.format      = (mFormat == AUDIO_FORMAT_PCM_16_BIT ? SNDRV_PCM_FORMAT_S16_LE : mFormat);
    //ToDo: Add conversion from channel Mask to channel count.
    if (mChannels == AUDIO_CHANNEL_OUT_MONO)
        alsa_handle.channels = MONO_CHANNEL_MODE;
    else
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.channels =  AudioSystem::popCount(mChannels);
    alsa_handle.sampleRate  = mSampleRate;
    alsa_handle.latency     = PLAYBACK_LATENCY;
    alsa_handle.rxHandle    = 0;
    alsa_handle.ucMgr       = mUcMgr;
    alsa_handle.session     = this;
    strlcpy(alsa_handle.useCase, useCase, sizeof(alsa_handle.useCase));

    mAlsaDevice->route(&alsa_handle, devices, mParent->mode());
    if (bIsUseCase) {
        snd_use_case_set(mUcMgr, "_verb", useCase);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", useCase);
    }

    //Set Tunnel or LPA bit if the playback over usb is tunnel or Lpa
    if((devices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
        (devices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
        if((!strcmp(useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
           (!strcmp(useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
            ALOGV("doRouting: LPA device switch to proxy");
            mParent->startUsbPlaybackIfNotStarted();
            mParent->musbPlaybackState |= USBPLAYBACKBIT_LPA;
        } else if((!strcmp(useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
            (!strcmp(useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
            ALOGD("doRouting: Tunnel Player device switch to proxy");
            mParent->startUsbPlaybackIfNotStarted();
            mParent->musbPlaybackState |= USBPLAYBACKBIT_TUNNEL;
        }
   }

    status = mAlsaDevice->open(&alsa_handle);
    if(status != NO_ERROR) {
        ALOGE("Could not open the ALSA device for use case %s", alsa_handle.useCase);
        mAlsaDevice->close(&alsa_handle);
    } else{
        mParent->mDeviceList.push_back(alsa_handle);
    }
    return status;
}

status_t AudioSessionOutALSA::closeDevice(alsa_handle_t *pHandle)
{
    status_t status = NO_ERROR;
    ALOGV("closeDevice: useCase %s", pHandle->useCase);
    //TODO: remove from mDeviceList
    if(pHandle) {
        status = mAlsaDevice->close(pHandle);
    }
    return status;
}

status_t AudioSessionOutALSA::setParameters(const String8& keyValuePairs)
{
    Mutex::Autolock autoLock(mLock);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    String8 value;

    int device;
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            ALOGV("setParameters(): keyRouting with device %#x", device);
            if (mParent->isExtOutDevice(device)) {
                mParent->mRouteAudioToExtOut = true;
                ALOGD("setParameters(): device %#x", device);
            }
            mParent->doRouting(device);
        }
        param.remove(key);
    }
    key = String8(AudioParameter::keyADSPStatus);
    if (param.get(key, value) == NO_ERROR) {
       if (value == "ONLINE"){
           mReachedEOS = true;
           mSkipWrite = true;
           mWriteCv.signal();
           mObserver->postEOS(1);
       }
       else if (value == "OFFLINE") {
           mParent->mLock.lock();
           requestAndWaitForEventThreadExit();
           mParent->mLock.unlock();
       }
    } else {
        mParent->setParameters(keyValuePairs);
    }

    return NO_ERROR;
}

String8 AudioSessionOutALSA::getParameters(const String8& keys)
{
    Mutex::Autolock autoLock(mLock);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mAlsaHandle->devices);
    }

    ALOGV("getParameters() %s", param.toString().string());
    return param.toString();
}

void AudioSessionOutALSA::reset() {
    mParent->mLock.lock();
    requestAndWaitForEventThreadExit();

#ifdef QCOM_USBAUDIO_ENABLED
    if (mParent->musbPlaybackState) {
        if((!strcmp(mAlsaHandle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
            (!strcmp(mAlsaHandle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
            ALOGV("Deregistering LPA bit: musbPlaybackState =%d",mParent->musbPlaybackState);
            mParent->musbPlaybackState &= ~USBPLAYBACKBIT_LPA;
        } else if((!strcmp(mAlsaHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
                 (!strcmp(mAlsaHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
            ALOGV("Deregistering Tunnel Player bit: musbPlaybackState =%d",mParent->musbPlaybackState);
            mParent->musbPlaybackState &= ~USBPLAYBACKBIT_TUNNEL;
        }
    }
#endif

    if(mAlsaHandle) {
        ALOGV("closeDevice mAlsaHandle");
        closeDevice(mAlsaHandle);
        mAlsaHandle = NULL;
    }
#ifdef QCOM_USBAUDIO_ENABLED
    mParent->closeUsbPlaybackIfNothingActive();
#endif
    ALOGV("Erase device list");
    for(ALSAHandleList::iterator it = mParent->mDeviceList.begin();
            it != mParent->mDeviceList.end(); ++it) {
        if((!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                            strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                            strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
           (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                            strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
           (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                            strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
            mParent->mDeviceList.erase(it);
            break;
        }
    }
    mParent->mLock.unlock();
}
void AudioSessionOutALSA::updateMetaData(size_t bytes) {
    mOutputMetadataTunnel.metadataLength = sizeof(mOutputMetadataTunnel);
    mOutputMetadataTunnel.timestamp = 0;
    mOutputMetadataTunnel.bufferLength =  bytes;
    ALOGD("bytes = %d , mAlsaHandle->handle->period_size = %d ",
            bytes, mAlsaHandle->handle->period_size);
}

status_t AudioSessionOutALSA::drainAndPostEOS_l()
{
    if (!mFilledQueue.empty()) {
        ALOGD("drainAndPostEOS called without empty mFilledQueue");
        return INVALID_OPERATION;
    }

    if (!mReachedEOS) {
        ALOGD("drainAndPostEOS called without mReachedEOS set");
        return INVALID_OPERATION;
    }

    if (mEosEventReceived) {
        ALOGD("drainAndPostEOS called after mEosEventReceived");
        return INVALID_OPERATION;
    }

    mSkipEOS = false;
    if ((!strncmp(mAlsaHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                  strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
       (!strncmp(mAlsaHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                  strlen(SND_USE_CASE_MOD_PLAY_TUNNEL)))) {
        ALOGD("Audio Drain DONE ++");
        mLock.unlock(); //to allow flush()
        int ret = ioctl(mAlsaHandle->handle->fd, SNDRV_COMPRESS_DRAIN);
        mLock.lock();

        if (ret < 0) {
            ret = -errno;
            ALOGE("Audio Drain failed with errno %s", strerror(errno));
            switch (ret) {
            case -EINTR: //interrupted by flush
            case -EWOULDBLOCK: //no writes given, drain would block indefintely
              mSkipEOS = true;
              break;
            default:
              mSkipEOS = false;
              break;
            }
        }
        ALOGD("Audio Drain DONE --");
    }

    if (mSkipEOS == false) {
        ALOGV("Posting the EOS to the observer player %p depending on mReachedEOS %d", \
              mObserver, mReachedEOS);
        mEosEventReceived = true;
        if (mObserver != NULL) {
          ALOGV("mObserver: posting EOS from eventcallback");
          mLock.unlock();
          mObserver->postEOS(0);
          mLock.lock();
        };
    } else {
      ALOGD("Ignored EOS posting since mSkipEOS is false");
    }
    return OK;
}

}       // namespace android_audio_legacy
