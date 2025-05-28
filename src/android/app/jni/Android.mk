LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := zlib_prebuilt
LOCAL_SRC_FILES := third_party/zlib/$(TARGET_ARCH_ABI)/lib/libz.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/third_party/zlib/$(TARGET_ARCH_ABI)/include
include $(PREBUILT_STATIC_LIBRARY)

LOCAL_MODULE    := libpng_prebuilt
LOCAL_SRC_FILES := third_party/libpng/$(TARGET_ARCH_ABI)/lib/libpng16.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/third_party/libpng/$(TARGET_ARCH_ABI)/include
LOCAL_STATIC_LIBRARIES := zlib_prebuilt
include $(PREBUILT_STATIC_LIBRARY)

LOCAL_MODULE    := atari800

A800_CORE_OBJS  := \
	afile.o \
	antic.o \
	artifact.o \
	atari.o \
	binload.o \
	cartridge.o \
	cartridge_info.o \
	cassette.o \
	cfg.o \
    codecs/audio.o \
    codecs/audio_adpcm.o \
    codecs/audio_mulaw.o \
    codecs/audio_pcm.o \
    codecs/container.o \
    codecs/container_avi.o \
    codecs/container_mp3.o \
    codecs/container_wav.o \
    codecs/image.o \
    codecs/image_pcx.o \
	codecs/image_png.o \
    codecs/video.o \
    codecs/video_mpng.o \
    codecs/video_mrle.o \
    codecs/video_zmbv.o \
	colours.o \
	colours_external.o \
	colours_pal.o \
	colours_ntsc.o \
	compfile.o \
	cpu.o \
	crc32.o \
	cycle_map.o \
	devices.o \
	esc.o \
	file_export.o \
	gtia.o \
	img_tape.o \
	input.o \
	log.o \
	memory.o \
	monitor.o \
	mzpokeysnd.o \
	pbi.o \
	pbi_bb.o \
	pbi_mio.o \
	pbi_scsi.o \
	pbi_xld.o \
	pia.o \
	pokey.o \
	pokeyrec.o \
	pokeysnd.o \
	remez.o \
	roms/altirra_5200_os.o \
	roms/altirra_5200_charset.o \
	roms/altirra_basic.o \
	roms/altirraos_800.o \
	roms/altirraos_xl.o \
	rtime.o \
	screen.o \
	sio.o \
	statesav.o \
	sysrom.o \
	ui.o \
	ui_basic.o \
	util.o \
	voicebox.o \
	votrax.o \
	votraxsnd.o
A800_CORE_LIBS   := -lz 

ANDROID_SRCS     := \
	androidinput.c \
	graphics.c \
	jni.c \
	platform.c \
	sound.c
ANDROID_LIBS     := -llog -lGLESv1_CM

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../.. \
	$(LOCAL_PATH)/../../../codecs \
	$(LOCAL_PATH)/third_party/zlib/$(TARGET_ARCH_ABI)/include \
    $(LOCAL_PATH)/third_party/libpng/$(TARGET_ARCH_ABI)/include

LOCAL_SRC_FILES  := $(A800_CORE_OBJS:%.o=../../../%.c) $(ANDROID_SRCS)
LOCAL_LDLIBS     := $(A800_CORE_LIBS) $(ANDROID_LIBS) -lOpenSLES
LOCAL_STATIC_LIBRARIES := \
    zlib_prebuilt \
    libpng_prebuilt

include $(BUILD_SHARED_LIBRARY)
