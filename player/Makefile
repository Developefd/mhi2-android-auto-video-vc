# Makefile for building stream-player for Harman MHI2 (QNX 6.5.0 / Tegra 3 ARMv7)
# Requires Docker and the MIB SDK with precompiled ffmpeg-mini.

DOCKER_IMAGE := registry.gitlab.com/andrewleech/mibsdk:latest
FFMPEG_PATH  ?= /work/VcMOSTRenderMqb/build/ffmpeg-mini
PWD          := $(shell pwd)

.PHONY: all clean

all: stream-player

stream-player: opengl_gpu.cc
	@echo "==> Building stream-player inside Docker SDK..."
	docker run --rm -v $(PWD)/../..:/work -w /work/mhi2-android-auto-video-vc/player $(DOCKER_IMAGE) sh -c '\
		. /etc/qnx/env && \
		qcc -Vgcc_ntoarmv7 -c -O2 -Wc,-Wall -I$(FFMPEG_PATH) -DNDEBUG \
			-I. -I/usr/qnx650/target/qnx6/usr/include \
			-EL -DVARIANT_le -DVARIANT_v7 -DBUILDENV_qss \
			opengl_gpu.cc -o opengl_gpu.o && \
		qcc -V4.4.2,gcc_ntoarmv7le opengl_gpu.o \
			-L/usr/qnx650/target/qnx6/armle-v7/lib -L/usr/qnx650/target/qnx6/armle-v7/usr/lib \
			-Wl,--start-group \
			$(FFMPEG_PATH)/libavformat/libavformat.a \
			$(FFMPEG_PATH)/libavcodec/libavcodec.a \
			$(FFMPEG_PATH)/libavutil/libavutil.a \
			-Bstatic -lbz2 -lz -lcpp -Bdynamic \
			-Wl,--end-group \
			-lEGL -lGLESv2 -lsocket -lm -lc \
			-o stream-player && \
		ntoarmv7-strip -s stream-player'
	@echo "==> Build successful: player/stream-player"

clean:
	rm -f opengl_gpu.o stream-player
