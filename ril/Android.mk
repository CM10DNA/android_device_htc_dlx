LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

ifeq ($(BOARD_PROVIDES_RIL),true)
LOCAL_SRC_FILES += HTCdlxQualcommRIL.java
endif

LOCAL_MODULE := dlxril
LOCAL_MODULE_TAGS := optional

include $(BUILD_JAVA_LIBRARY)
