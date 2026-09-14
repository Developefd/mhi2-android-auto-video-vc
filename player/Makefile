# Makefile for building stream-player for Harman MHI2 (QNX 6.5.0 / Tegra 3 ARMv7)
# Requires Docker and the MIB SDK image; ffmpeg-mini is cross-compiled
# automatically by build_ffmpeg.sh the first time it's needed.

DOCKER_IMAGE  := registry.gitlab.com/andrewleech/mibsdk:latest
PWD           := $(shell pwd)
FFMPEG_PATH   ?= $(PWD)/build/ffmpeg-mini
# opengl_gpu.cc and build_ffmpeg.sh are self-contained within player/, so
# only this directory (not the whole repo) needs to be mounted into the
# container, as /work. That keeps host path (FFMPEG_PATH, used by make
# itself for dependency checks) and container path (used inside the
# docker sh -c recipes below) a trivial prefix swap of one another --
# swapping in a wider mount here previously caused make install inside
# the container to land outside the bind mount and vanish with --rm.
FFMPEG_PATH_DOCKER := $(subst $(PWD),/work,$(FFMPEG_PATH))
FFMPEG_STAMP  := $(FFMPEG_PATH)/.built
FFMPEG_LIBS   := $(FFMPEG_PATH)/libavformat/libavformat.a \
                  $(FFMPEG_PATH)/libavcodec/libavcodec.a \
                  $(FFMPEG_PATH)/libavutil/libavutil.a

.PHONY: all ffmpeg clean ffmpeg-clean

all: stream-player

# ffmpeg-mini is expensive to build, so it's cached under build/ and only
# rebuilt when missing; `make ffmpeg-clean` forces a rebuild. All three
# libraries come out of one build_ffmpeg.sh run, so they're gated behind
# a single stamp file rather than each being its own rule (which would
# otherwise re-run the whole cross-compile once per missing library).
ffmpeg: $(FFMPEG_STAMP)

$(FFMPEG_LIBS): $(FFMPEG_STAMP)

$(FFMPEG_STAMP):
	@echo "==> ffmpeg-mini not found, cross-compiling it inside Docker SDK..."
	docker run --rm -v $(PWD):/work -w /work $(DOCKER_IMAGE) sh -c '\
		. /etc/qnx/env && \
		FFMPEG_PATH=$(FFMPEG_PATH_DOCKER) ./build_ffmpeg.sh'
	@touch $(FFMPEG_STAMP)

stream-player: opengl_gpu.cc $(FFMPEG_LIBS)
	@echo "==> Building stream-player inside Docker SDK..."
	docker run --rm -v $(PWD):/work -w /work $(DOCKER_IMAGE) sh -c '\
		. /etc/qnx/env && \
		qcc -Vgcc_ntoarmv7 -c -O2 -Wc,-Wall -I$(FFMPEG_PATH_DOCKER) -DNDEBUG \
			-I. -I/usr/qnx650/target/qnx6/usr/include \
			-EL -DVARIANT_le -DVARIANT_v7 -DBUILDENV_qss \
			opengl_gpu.cc -o opengl_gpu.o && \
		qcc -V4.4.2,gcc_ntoarmv7le opengl_gpu.o \
			-L/usr/qnx650/target/qnx6/armle-v7/lib -L/usr/qnx650/target/qnx6/armle-v7/usr/lib \
			-Wl,--start-group \
			$(FFMPEG_PATH_DOCKER)/libavformat/libavformat.a \
			$(FFMPEG_PATH_DOCKER)/libavcodec/libavcodec.a \
			$(FFMPEG_PATH_DOCKER)/libavutil/libavutil.a \
			-Bstatic -lbz2 -lz -lcpp -Bdynamic \
			-Wl,--end-group \
			-lEGL -lGLESv2 -lsocket -lm -lc \
			-o stream-player && \
		ntoarmv7-strip -s stream-player'
	@echo "==> Build successful: player/stream-player"

clean:
	rm -f opengl_gpu.o stream-player

ffmpeg-clean:
	rm -rf build
