#
# Copyright (C) 2011 The Android Open Source Project
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

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    VideoEditorVideoDecoder.cpp \
    VideoEditorAudioDecoder.cpp \
    VideoEditorMp3Reader.cpp \
    VideoEditor3gpReader.cpp \
    VideoEditorUtils.cpp \
    VideoEditorBuffer.c \
    VideoEditorVideoEncoder.cpp \
    VideoEditorAudioEncoder.cpp

LOCAL_C_INCLUDES += \
    $(TOP)/frameworks/base/core/jni \
    $(TOP)/frameworks/base/include \
    $(TOP)/frameworks/av/include/media \
    $(TOP)/frameworks/av/media/libmediaplayerservice \
    $(TOP)/frameworks/av/media/libstagefright \
    $(TOP)/frameworks/av/media/libstagefright/include \
    $(TOP)/frameworks/av/media/libstagefright/rtsp \
    $(JNI_H_INCLUDE) \
    $(call include-path-for, corecg graphics) \
    $(TOP)/frameworks/native/include/media/openmax \
    $(TOP)/frameworks/base/core/jni/mediaeditor \
    $(TOP)/frameworks/av/libvideoeditor/vss/inc \
    $(TOP)/frameworks/av/libvideoeditor/vss/common/inc \
    $(TOP)/frameworks/av/libvideoeditor/vss/mcs/inc \
    $(TOP)/frameworks/av/libvideoeditor/lvpp \
    $(TOP)/frameworks/av/libvideoeditor/osal/inc \
    $(TOP)/frameworks/av/libvideoeditor/include \
    $(TOP)/frameworks/av/libvideoeditor/vss/stagefrightshells/inc \
    $(TOP)/vendor/qcom/opensource/omx/mm-core/inc

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libandroid_runtime \
    libnativehelper \
    libmedia \
    libbinder \
    libstagefright \
    libstagefright_omx \
    libgui \
    libvideoeditorplayer \
    libvideoeditor_osal \
    libdl

LOCAL_CFLAGS += \



LOCAL_STATIC_LIBRARIES := \
    libstagefright_color_conversion


LOCAL_MODULE:= libvideoeditor_stagefrightshells

LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
