#
# Copyright (C) 2011 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# common msm8960 configs
$(call inherit-product, device/htc/msm8960-common/msm8960.mk)

# Inherit from dlx device
$(call inherit-product, device/htc/dlx/device.mk)

# The gps config appropriate for this device
PRODUCT_COPY_FILES += device/common/gps/gps.conf_US_SUPL:system/etc/gps.conf

# Ramdisk
PRODUCT_PACKAGES += \
    fstab.dlx \
    init.qcom.firmware_links.sh \
    init.qcom.sh \
    init.dlx.rc \
    init.dlx.usb.rc \
    ueventd.dlx.rc

PRODUCT_PACKAGES += \
    libnetcmdiface

# Recovery
PRODUCT_PACKAGES += \
    init.recovery.dlx.rc \
    choice_fn \
    detect_key \
    offmode_charging \
    power_test

# Get the sample verizon list of APNs
PRODUCT_COPY_FILES += device/sample/etc/apns-conf_verizon.xml:system/etc/apns-conf.xml

# NFCEE access control
ifeq ($(TARGET_BUILD_VARIANT),user)
    NFCEE_ACCESS_PATH := device/htc/dlx/configs/nfcee_access.xml
else
    NFCEE_ACCESS_PATH := device/htc/dlx/configs/nfcee_access_debug.xml
endif
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:system/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.audio.low_latency.xml:system/etc/permissions/android.hardware.audio.low_latency.xml \
    $(NFCEE_ACCESS_PATH):system/etc/nfcee_access.xml \
    frameworks/base/nfc-extras/com.android.nfc_extras.xml:system/etc/permissions/com.android.nfc_extras.xml \
    frameworks/native/data/etc/android.hardware.nfc.xml:system/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:system/etc/permissions/android.hardware.wifi.direct.xml

# Media configs
PRODUCT_COPY_FILES += device/htc/dlx/configs/AudioBTID.csv:system/etc/AudioBTID.csv
PRODUCT_COPY_FILES += device/htc/dlx/configs/AudioBTIDnew.csv:system/etc/AudioBTIDnew.csv

# wifi config
PRODUCT_COPY_FILES += \
    device/htc/dlx/configs/calibration:/system/etc/calibration \
    device/htc/dlx/configs/calibration.gpio4:/system/etc/calibration.gpio4

# Audio config
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/audio_policy.conf:system/etc/audio_policy.conf

# Sound configs
PRODUCT_COPY_FILES += \
    device/htc/dlx/dsp/soundimage/srs_bypass.cfg:system/etc/soundimage/srs_bypass.cfg \
    device/htc/dlx/dsp/soundimage/srsfx_trumedia_51.cfg:system/etc/soundimage/srsfx_trumedia_51.cfg \
    device/htc/dlx/dsp/soundimage/srsfx_trumedia_movie.cfg:system/etc/soundimage/srsfx_trumedia_movie.cfg \
    device/htc/dlx/dsp/soundimage/srsfx_trumedia_music.cfg:system/etc/soundimage/srsfx_trumedia_music.cfg \
    device/htc/dlx/dsp/soundimage/srsfx_trumedia_voice.cfg:system/etc/soundimage/srsfx_trumedia_voice.cfg \
    device/htc/dlx/dsp/soundimage/srs_geq10.cfg:system/etc/soundimage/srs_geq10.cfg \
    device/htc/dlx/dsp/soundimage/srs_global.cfg:system/etc/soundimage/srs_global.cfg

PRODUCT_COPY_FILES += \
    device/htc/dlx/dsp/snd_soc_msm/snd_soc_msm_2x_Fusion3:/system/etc/snd_soc_msm/snd_soc_msm_2x_Fusion3 

# Keylayouts and Keychars
PRODUCT_COPY_FILES += \
    device/htc/dlx/keylayout/AVRCP.kl:system/usr/keylayout/AVRCP.kl \
    device/htc/dlx/keylayout/dummy_keypad.kl:system/usr/keylayout/dummy_keypad.kl \
    device/htc/dlx/keylayout/Generic.kl:system/usr/keylayout/Generic.kl \
    device/htc/dlx/keylayout/projector-Keypad.kl:system/usr/keylayout/projector-Keypad.kl \
    device/htc/dlx/keylayout/h2w_headset.kl:system/usr/keylayout/h2w_headset.kl \
    device/htc/dlx/keylayout/keypad_8960.kl:system/usr/keylayout/keypad_8960.kl \
    device/htc/dlx/keylayout/msm8960-snd-card_Button_Jack.kl:system/usr/keylayout/msm8960-snd-card_Button_Jack.kl \
    device/htc/dlx/keylayout/qwerty.kl:system/usr/keylayout/qwerty.kl \
    device/htc/dlx/keylayout/synaptics-rmi-touchscreen.kl:system/usr/keylayout/synaptics-rmi-touchscreen.kl \
    device/htc/dlx/keylayout/Vendor_045e_Product_028e.kl:system/usr/keylayout/Vendor_045e_Product_028e.kl \
    device/htc/dlx/keylayout/Vendor_046d_Product_c216.kl:system/usr/keylayout/Vendor_046d_Product_c216.kl \
    device/htc/dlx/keylayout/Vendor_046d_Product_c294.kl:system/usr/keylayout/Vendor_046d_Product_c294.kl \
    device/htc/dlx/keylayout/Vendor_046d_Product_c299.kl:system/usr/keylayout/Vendor_046d_Product_c299.kl \
    device/htc/dlx/keylayout/Vendor_046d_Product_c532.kl:system/usr/keylayout/Vendor_046d_Product_c532.kl \
    device/htc/dlx/keylayout/Vendor_054c_Product_0268.kl:system/usr/keylayout/Vendor_054c_Product_0268.kl \
    device/htc/dlx/keylayout/Vendor_05ac_Product_0239.kl:system/usr/keylayout/Vendor_05ac_Product_0239.kl \
    device/htc/dlx/keylayout/Vendor_22b8_Product_093d.kl:system/usr/keylayout/Vendor_22b8_Product_093d.kl 

# Input device config
PRODUCT_COPY_FILES += \
    device/htc/dlx/idc/projector_input.idc:system/usr/idc/projector_input.idc \
    device/htc/dlx/idc/qwerty2.idc:system/usr/idc/qwerty2.idc \
    device/htc/dlx/idc/qwerty.idc:system/usr/idc/qwerty.idc \
    device/htc/dlx/idc/synaptics-rmi-touchscreen.idc:system/usr/idc/synaptics-rmi-touchscreen.idc

# Camera
PRODUCT_PACKAGES += \
	camera.msm8960

# GPS
PRODUCT_PACKAGES += \
        libloc_adapter \
        libloc_eng \
        libloc_api_v02 \
        libgps.utils \
        gps.msm8960

# NFC
PRODUCT_PACKAGES += \
    nfc.msm8960 \
    libnfc \
    libnfc_ndef \
    libnfc_jni \
    Nfc \
    Tag \
    com.android.nfc_extras

# Torch
PRODUCT_PACKAGES += \
    Torch

# Permissions
PRODUCT_COPY_FILES += \
        frameworks/native/data/etc/android.hardware.telephony.cdma.xml:system/etc/permissions/android.hardware.telephony.cdma.xml \
        frameworks/native/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml

# Increase the HWUI font cache since we have tons of RAM
PRODUCT_PROPERTY_OVERRIDES += \
    ro.hwui.text_cache_width=2048

# Extra properties
PRODUCT_PROPERTY_OVERRIDES += \
    ro.setupwizard.enable_bypass=1 \
    dalvik.vm.lockprof.threshold=500 \
    ro.com.google.locationfeatures=1 \
    dalvik.vm.dexopt-flags=m=y \
    ro.com.google.clientidbase=android-htc \
    ro.com.google.clientidbase.yt=android-verizon \
    ro.com.google.clientidbase.am=android-verizon \
    ro.com.google.clientidbase.gmm=android-htc \
    ro.com.google.clientidbase.ms=android-verizon \
    gsm.sim.operator.alpha = Verizon \
    gsm.sim.operator.numeric = 311480 \
    gsm.sim.operator.iso-country = us \
    gsm.operator.alpha = Verizon \
    gsm.operator.numeric = 311480 \
    gsm.operator.iso-country = us \
    ro.cdma.home.operator.alpha = Verizon \
    ro.cdma.home.operator.numeric = 311480 \
    ro.cdma.data_retry_config=max_retries=infinite,0,0,60000,120000,480000,900000 \
    ro.ril.set.mtusize=1428 \
    persist.radio.snapshot_enabled=1 \
    persist.radio.snapshot_timer=22 \
    ro.config.multimode_cdma=1 \
    ro.config.combined_signal=true \
    ro.gsm.data_retry_config=max_retries=infinite,5000,5000,60000,120000,480000,900000 \
    ro.opengles.version=196608 \
    persist.eons.enabled=false

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=mtp

# We have enough space to hold precise GC data
PRODUCT_TAGS += dalvik.gc.type-precise

PRODUCT_CHARACTERISTICS := nosdcard

# Set build date
PRODUCT_BUILD_PROP_OVERRIDES += BUILD_UTC_DATE=0

# Device uses high-density artwork where available
PRODUCT_AAPT_CONFIG := normal hdpi xhdpi xxhdpi
PRODUCT_AAPT_PREF_CONFIG := xhdpi xxhdpi
PRODUCT_LOCALES += en_US

# call dalvik heap config
$(call inherit-product-if-exists, frameworks/native/build/phone-xxhdpi-2048-dalvik-heap.mk)

# call hwui memory config
$(call inherit-product-if-exists, frameworks/native/build/phone-xxhdpi-2048-hwui-memory.mk)
