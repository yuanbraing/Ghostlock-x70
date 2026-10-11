# ============================================================================
#  配置
# ============================================================================
API        ?= 35
PROJECT    ?= HNMTN-Q-HONORMTN-AN00
OUTDIR     ?= build/$(PROJECT)/bin
EMBEDDIR   ?= build/embed
DIRECT_KSU ?= 1

TARGET_DIR    := src/targets/$(PROJECT)
TARGET_HEADER := $(TARGET_DIR)/target.h

ifeq ($(wildcard $(TARGET_HEADER)),)
$(error unknown PROJECT=$(PROJECT), missing $(TARGET_HEADER))
endif

# ============================================================================
#  源文件
# ============================================================================
# 优先使用 src/targets/$(PROJECT)/<file>，否则回退到 src/<file>
pick_src = $(if $(wildcard $(TARGET_DIR)/$(1)),$(TARGET_DIR)/$(1),src/$(1))

CORE_SRCS := \
    $(call pick_src,cpubind.c) \
    $(call pick_src,util.c) \
    $(call pick_src,main.c) \
    $(call pick_src,module.c) \
    $(call pick_src,poll.S) \
    $(call pick_src,pipe.c) \
    $(call pick_src,kaslr_crack.c) \
    $(call pick_src,root.c)

# DIRECT_KSU 差异集中到三个变量，构建规则统一
ifeq ($(DIRECT_KSU),0)
BLOB_SRCS   := src/su_blob.S src/wallpaper_blob.S
BLOB_DEPS   := $(EMBEDDIR)/su_daemon_aarch64_pie assets/wallpaper.webp
BLOB_CFLAGS :=
else
BLOB_SRCS   := src/ksu_blob.S
BLOB_DEPS   := assets/ksud
BLOB_CFLAGS := -DDIRECT_KSU
endif

PRELOAD_SRCS := $(CORE_SRCS) src/preload.c $(BLOB_SRCS)
PRELOAD      := $(OUTDIR)/preload
EMBED_SU     := $(EMBEDDIR)/su_daemon_aarch64_pie

ALL_HEADERS := \
    $(TARGET_HEADER) \
    $(TARGET_DIR)/struct.h \
    src/common.h \
    src/cpubind.h \
    src/module.h \
    src/offset.h \
    src/kernelsnitch/kernelsnitch.h \
    src/kernelsnitch/utils.h \
    src/kernelsnitch/futex_hash.h \
    src/kernelsnitch/timeutils.h

# ============================================================================
#  工具链
# ============================================================================
DEFAULT_NDK_ROOT := $(HOME)/androids/ndk-r29
NDK_ROOT ?= $(or $(ANDROID_NDK_HOME),$(ANDROID_NDK_ROOT),$(wildcard $(DEFAULT_NDK_ROOT)))
NDK_TOOLCHAIN ?= $(if $(NDK_ROOT),$(NDK_ROOT)/toolchains/llvm/prebuilt/linux-aarch64)
NDK_CC    := $(NDK_TOOLCHAIN)/bin/aarch64-linux-android$(API)-clang
HOST_CLANG ?= clang
SYSROOT   ?= $(if $(NDK_TOOLCHAIN),$(NDK_TOOLCHAIN)/sysroot)
RESOURCE_DIR ?= $(if $(NDK_TOOLCHAIN),$(NDK_TOOLCHAIN)/lib/clang/21)

HOST_TARGET_FLAGS := \
    --target=aarch64-linux-android$(API) \
    --sysroot=$(SYSROOT) \
    -resource-dir $(RESOURCE_DIR) \
    --rtlib=compiler-rt \
    --unwindlib=none

HOST_COMMON_LDFLAGS := \
    -fuse-ld=lld \
    -Wl,-rpath-link,$(SYSROOT)/usr/lib/aarch64-linux-android/$(API) \
    -L$(SYSROOT)/usr/lib/aarch64-linux-android/$(API) \
    -L$(SYSROOT)/usr/lib/aarch64-linux-android

HOST_PIE_LDFLAGS := \
    $(HOST_COMMON_LDFLAGS) \
    -Wl,-dynamic-linker,/system/bin/linker64

# 工具链优先级：用户 CC > NDK clang > 宿主 clang + 目标标志
ifneq ($(origin CC),default)
    TARGET_CC             := $(CC)
    TARGET_FLAGS          :=
    TARGET_COMMON_LDFLAGS :=
    TARGET_PIE_LDFLAGS    :=
else ifneq ($(wildcard $(NDK_CC)),)
    NDK_CC_WORKS := $(shell $(NDK_CC) --version >/dev/null 2>&1 && echo yes)
    ifeq ($(NDK_CC_WORKS),yes)
        TARGET_CC             := $(NDK_CC)
        TARGET_FLAGS          :=
        TARGET_COMMON_LDFLAGS :=
        TARGET_PIE_LDFLAGS    :=
    else
        TARGET_CC             := $(HOST_CLANG)
        TARGET_FLAGS          := $(HOST_TARGET_FLAGS)
        TARGET_COMMON_LDFLAGS := $(HOST_COMMON_LDFLAGS)
        TARGET_PIE_LDFLAGS    := $(HOST_PIE_LDFLAGS)
    endif
else
    TARGET_CC             := $(HOST_CLANG)
    TARGET_FLAGS          := $(HOST_TARGET_FLAGS)
    TARGET_COMMON_LDFLAGS := $(HOST_COMMON_LDFLAGS)
    TARGET_PIE_LDFLAGS    := $(HOST_PIE_LDFLAGS)
endif

# ============================================================================
#  编译标志
# ============================================================================
COMMON_CFLAGS := -O2 -g0 -Wall -Wextra -Isrc $(EXTRA_CFLAGS)
PIE_CFLAGS    := -fPIE -pie $(COMMON_CFLAGS)
WARN_CFLAGS   := -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
TARGET_CFLAGS := -DTARGET_CONFIG_H=\"targets/$(PROJECT)/target.h\"

# ============================================================================
#  构建
# ============================================================================
.DEFAULT_GOAL := preload
.PHONY: all preload clean info list-projects install run

all: preload
preload: $(PRELOAD)

$(OUTDIR):
	@mkdir -p $@

ifeq ($(DIRECT_KSU),0)
$(EMBEDDIR):
	@mkdir -p $@

$(EMBED_SU): src/su_daemon.c | $(EMBEDDIR)
	$(TARGET_CC) $(TARGET_FLAGS) $(PIE_CFLAGS) $(TARGET_CFLAGS) \
	    $< $(TARGET_PIE_LDFLAGS) -o $@
endif

# 统一构建规则：DIRECT_KSU 的差异通过 BLOB_* 变量注入
$(PRELOAD): $(PRELOAD_SRCS) $(BLOB_DEPS) $(ALL_HEADERS) | $(OUTDIR)
	$(TARGET_CC) $(TARGET_FLAGS) $(WARN_CFLAGS) $(TARGET_CFLAGS) $(BLOB_CFLAGS) \
	    $(PRELOAD_SRCS) $(TARGET_COMMON_LDFLAGS) \
	    -o $@ -pthread
	@sha256sum $@

# ============================================================================
#  信息
# ============================================================================
info:
	@echo "PROJECT               = $(PROJECT)"
	@echo "TARGET_DIR            = $(TARGET_DIR)"
	@echo "DIRECT_KSU            = $(DIRECT_KSU)"
	@echo "TARGET_CC             = $(TARGET_CC)"
	@echo "TARGET_FLAGS          = $(TARGET_FLAGS)"
	@echo "TARGET_COMMON_LDFLAGS = $(TARGET_COMMON_LDFLAGS)"
	@echo "TARGET_PIE_LDFLAGS    = $(TARGET_PIE_LDFLAGS)"
	@echo "PRELOAD               = $(PRELOAD)"
	@echo "EMBED_SU              = $(EMBED_SU)"
	@echo "BLOB_SRCS             = $(BLOB_SRCS)"
	@echo "BLOB_DEPS             = $(BLOB_DEPS)"
	@echo "CORE_SRCS             = $(CORE_SRCS)"
	@echo "PRELOAD_SRCS          = $(PRELOAD_SRCS)"

list-projects:
	@find src/targets -mindepth 2 -maxdepth 2 -name target.h -printf '%h\n' \
	    | sed 's#src/targets/##' | sort

# ============================================================================
#  清理与部署
# ============================================================================
clean:
	rm -rf build

install: preload
	adb push $(PRELOAD) /data/local/tmp/

run: preload
	adb push $(PRELOAD) /data/local/tmp/
	adb shell /data/local/tmp/preload