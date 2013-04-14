/* ALSAStreamOps.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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

#define LOG_TAG "ALSAStreamOps"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>
#include "AudioUtil.h"
#include "AudioHardwareALSA.h"

namespace android_audio_legacy
{

// unused 'enumVal;' is to catch error at compile time if enumVal ever changes
// or applied on a non-existent enum
#define ENUM_TO_STRING(var, enumVal) {var = #enumVal; enumVal;}

// ----------------------------------------------------------------------------

ALSAStreamOps::ALSAStreamOps(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    mParent(parent),
    mHandle(handle)
{
}

ALSAStreamOps::~ALSAStreamOps()
{
    Mutex::Autolock autoLock(mParent->mLock);

    if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
       (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
        if((mParent->mVoipStreamCount)) {
            mParent->mVoipStreamCount--;
            if(mParent->mVoipStreamCount > 0) {
                ALOGD("ALSAStreamOps::close() Ignore");
                return ;
            }
       }
       mParent->mVoipStreamCount = 0;
       mParent->mVoipMicMute = 0;
       mParent->mVoipBitRate = 0;
    }
    close();

    for(ALSAHandleList::iterator it = mParent->mDeviceList.begin();
            it != mParent->mDeviceList.end(); ++it) {
            if (mHandle == &(*it)) {
                it->useCase[0] = 0;
                mParent->mDeviceList.erase(it);
                break;
            }
    }
}

// use emulated popcount optimization
// http://www.df.lth.se/~john_e/gems/gem002d.html
static inline uint32_t popCount(uint32_t u)
{
    u = ((u&0x55555555) + ((u>>1)&0x55555555));
    u = ((u&0x33333333) + ((u>>2)&0x33333333));
    u = ((u&0x0f0f0f0f) + ((u>>4)&0x0f0f0f0f));
    u = ((u&0x00ff00ff) + ((u>>8)&0x00ff00ff));
    u = ( u&0x0000ffff) + (u>>16);
    return u;
}

status_t ALSAStreamOps::set(int      *format,
                            uint32_t *channels,
                            uint32_t *rate,
                            uint32_t device)
{
    mDevices = device;
    if (channels && *channels != 0) {
        if (mHandle->channels != popCount(*channels))
            return BAD_VALUE;
    } else if (channels) {
        *channels = 0;
        if (mHandle->devices & AudioSystem::DEVICE_OUT_ALL) {
            switch(mHandle->channels) {
                case 8:
                case 7:
                case 6:
                case 5:
                    *channels |= audio_channel_out_mask_from_count(mHandle->channels);
                    break;
                    // Do not fall through
                case 4:
                    *channels |= AUDIO_CHANNEL_OUT_BACK_LEFT;
                    *channels |= AUDIO_CHANNEL_OUT_BACK_RIGHT;
                    // Fall through...
                default:
                case 2:
                    *channels |= AUDIO_CHANNEL_OUT_FRONT_RIGHT;
                    // Fall through...
                case 1:
                    *channels |= AUDIO_CHANNEL_OUT_FRONT_LEFT;
                    break;
            }
        } else {
            switch(mHandle->channels) {
#ifdef QCOM_SSR_ENABLED
                // For 5.1 recording
                case 6 :
                    *channels |= AUDIO_CHANNEL_IN_5POINT1;
                    break;
#endif
                    // Do not fall through...
                default:
                case 2:
                    *channels |= AUDIO_CHANNEL_IN_RIGHT;
                    // Fall through...
                case 1:
                    *channels |= AUDIO_CHANNEL_IN_LEFT;
                    break;
            }
        }
    }

    if (rate && *rate > 0) {
        if (mHandle->sampleRate != *rate)
            return BAD_VALUE;
    } else if (rate) {
        *rate = mHandle->sampleRate;
    }

    snd_pcm_format_t iformat = mHandle->format;

    if (format) {
        switch(*format) {
            case AUDIO_FORMAT_DEFAULT:
                break;

            case AUDIO_FORMAT_PCM_16_BIT:
                iformat = SNDRV_PCM_FORMAT_S16_LE;
                break;
            case AUDIO_FORMAT_AMR_NB:
            case AUDIO_FORMAT_AMR_WB:
#ifdef QCOM_AUDIO_FORMAT_ENABLED
            case AUDIO_FORMAT_EVRC:
            case AUDIO_FORMAT_EVRCB:
            case AUDIO_FORMAT_EVRCWB:
#endif
                iformat = *format;
                break;

            case AUDIO_FORMAT_PCM_8_BIT:
                iformat = SNDRV_PCM_FORMAT_S8;
                break;

            default:
                ALOGE("Unknown PCM format %i. Forcing default", *format);
                break;
        }

        if (mHandle->format != iformat)
            return BAD_VALUE;

        switch(iformat) {
            case SNDRV_PCM_FORMAT_S16_LE:
                *format = AUDIO_FORMAT_PCM_16_BIT;
                break;
            case SNDRV_PCM_FORMAT_S8:
                *format = AUDIO_FORMAT_PCM_8_BIT;
                break;
            default:
                break;
        }
    }

    return NO_ERROR;
}

status_t ALSAStreamOps::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting),value;
    int device;
    status_t err = NO_ERROR;

#ifdef SEPERATED_AUDIO_INPUT
    String8 key_input = String8(AudioParameter::keyInputSource);
    int source;

    if (param.getInt(key_input, source) == NO_ERROR) {
        ALOGD("setParameters(), input_source = %d", source);
        mParent->mALSADevice->setInput(source);
        param.remove(key_input);
    }
#endif

    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        ALOGD("setParameters(): keyRouting with device 0x%x", device);
        if(device) {
            ALOGD("setParameters(): keyRouting with device %#x", device);
            if (mParent->isExtOutDevice(device)) {
                mParent->mRouteAudioToExtOut = true;
                ALOGD("setParameters(): device %#x", device);
            }
            err = mParent->doRouting(device);
            if(err) {
                ALOGE("doRouting failed = %d",err);
            }
            else {
                mDevices = device;
            }
        } else {
            ALOGE("must not change mDevices to 0");
        }
        param.remove(key);
    }
#ifdef QCOM_FM_ENABLED
    else {
        key = String8(AudioParameter::keyHandleFm);
        if (param.getInt(key, device) == NO_ERROR) {
            ALOGD("setParameters(): handleFm with device %d", device);
            if(device) {
                mDevices = device;
                mParent->handleFm(device);
            }
            param.remove(key);
        } else {
            mParent->setParameters(keyValuePairs);
        }
    }
#endif

    return err;
}

String8 ALSAStreamOps::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mDevices);
    }
    else {
        key = String8(VOIPCHECK_KEY);
        if (param.get(key, value) == NO_ERROR) {
            if((!strncmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL, strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
               (!strncmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP, strlen(SND_USE_CASE_MOD_PLAY_VOIP))))
                param.addInt(key, true);
            else
                param.addInt(key, false);
        }
    }
    key = String8(AUDIO_PARAMETER_STREAM_SUP_CHANNELS);
    if (param.get(key, value) == NO_ERROR) {
        EDID_AUDIO_INFO info = { 0 };
        bool first = true;
        value = String8();
        if (AudioUtil::getHDMIAudioSinkCaps(&info)) {
            for (int i = 0; i < info.nAudioBlocks && i < MAX_EDID_BLOCKS; i++) {
                String8 append;
                switch (info.AudioBlocksArray[i].nChannels) {
                //Do not handle stereo output in Multi-channel cases
                //Stereo case is handled in normal playback path
                case 6:
                    ENUM_TO_STRING(append, AUDIO_CHANNEL_OUT_5POINT1);
                    break;
                case 8:
                    ENUM_TO_STRING(append, AUDIO_CHANNEL_OUT_7POINT1);
                    break;
                default:
                    ALOGD("Unsupported number of channels %d", info.AudioBlocksArray[i].nChannels);
                    break;
                }
                if (!append.isEmpty()) {
                    value += (first ? append : String8("|") + append);
                    first = false;
                }
            }
        } else {
            ALOGE("Failed to get HDMI sink capabilities");
        }
        param.add(key, value);
    }
    ALOGV("getParameters() %s", param.toString().string());
    return param.toString();
}

uint32_t ALSAStreamOps::sampleRate() const
{
    return mHandle->sampleRate;
}

//
// Return the number of bytes (not frames)
//
size_t ALSAStreamOps::bufferSize() const
{
    ALOGV("bufferSize() returns %d", mHandle->bufferSize);
    return mHandle->bufferSize;
}

int ALSAStreamOps::format() const
{
    int audioSystemFormat;

    snd_pcm_format_t ALSAFormat = mHandle->format;

    switch(ALSAFormat) {
        case SNDRV_PCM_FORMAT_S8:
             audioSystemFormat = AUDIO_FORMAT_PCM_8_BIT;
             break;

        case AUDIO_FORMAT_AMR_NB:
        case AUDIO_FORMAT_AMR_WB:
#ifdef QCOM_AUDIO_FORMAT_ENABLED
        case AUDIO_FORMAT_EVRC:
        case AUDIO_FORMAT_EVRCB:
        case AUDIO_FORMAT_EVRCWB:
#endif
            audioSystemFormat = mHandle->format;
            break;
        case SNDRV_PCM_FORMAT_S16_LE:
            audioSystemFormat = AUDIO_FORMAT_PCM_16_BIT;
            break;

        default:
            LOG_FATAL("Unknown AudioSystem bit width %d!", audioSystemFormat);
            audioSystemFormat = AUDIO_FORMAT_PCM_16_BIT;
            break;
    }

    ALOGV("ALSAFormat:0x%x,audioSystemFormat:0x%x",ALSAFormat,audioSystemFormat);
    return audioSystemFormat;
}

uint32_t ALSAStreamOps::channels() const
{
    unsigned int count = mHandle->channels;
    uint32_t channels = 0;

    if (mDevices & AudioSystem::DEVICE_OUT_ALL)
        switch(count) {
            case 8:
            case 7:
            case 6:
            case 5:
                channels |=audio_channel_out_mask_from_count(count);
                break;
                // Do not fall through
            case 4:
                channels |= AUDIO_CHANNEL_OUT_BACK_LEFT;
                channels |= AUDIO_CHANNEL_OUT_BACK_RIGHT;
                // Fall through...
            default:
            case 2:
                channels |= AUDIO_CHANNEL_OUT_FRONT_RIGHT;
                // Fall through...
            case 1:
                channels |= AUDIO_CHANNEL_OUT_FRONT_LEFT;
                break;
        }
    else
        switch(count) {
#ifdef QCOM_SSR_ENABLED
            // For 5.1 recording
            case 6 :
                channels |= AUDIO_CHANNEL_IN_5POINT1;
                break;
                // Do not fall through...
#endif
            default:
            case 2:
                channels |= AUDIO_CHANNEL_IN_RIGHT;
                // Fall through...
            case 1:
                channels |= AUDIO_CHANNEL_IN_LEFT;
                break;
        }

    return channels;
}

void ALSAStreamOps::close()
{
    ALOGD("close");
    if((!strncmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL, strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
       (!strncmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP, strlen(SND_USE_CASE_MOD_PLAY_VOIP)))) {
       mParent->mVoipMicMute = false;
       mParent->mVoipBitRate = 0;
       mParent->mVoipStreamCount = 0;
    }
    mParent->mALSADevice->close(mHandle);
}

//
// Set playback or capture PCM device.  It's possible to support audio output
// or input from multiple devices by using the ALSA plugins, but this is
// not supported for simplicity.
//
// The AudioHardwareALSA API does not allow one to set the input routing.
//
// If the "routes" value does not map to a valid device, the default playback
// device is used.
//
status_t ALSAStreamOps::open(int mode)
{
    ALOGD("open");
    return mParent->mALSADevice->open(mHandle);
}

}       // namespace androidi_audio_legacy
