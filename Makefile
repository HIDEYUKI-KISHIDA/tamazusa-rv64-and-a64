# TK2-RV64 QEMU virt — Phase 1-4 bring-up (default) or official TK2 link (TK2=1)
#
#   make              # bring-up: tk2-rv64.elf
#   make TK2=1        # link tkernel_2 (M5b equivalent)
#   make TK2=1 qemu
#   make TK2=1 SMODE=1        # 構成 B: S-mode + OpenSBI（0x80200000）
#   make TK2=1 SMODE=1 qemu
#
CROSS   ?= riscv64-unknown-elf
CC      := $(CROSS)-gcc
LD      := $(CROSS)-ld
OBJCOPY := $(CROSS)-objcopy
SIZE    := $(CROSS)-size

ARCH    := -march=rv64gc -mabi=lp64d -mcmodel=medany
BASE_CFLAGS := $(ARCH) -ffreestanding -fno-builtin -fno-pic -fno-stack-protector \
               -Wall -Wextra -O1 -g

TK2 ?= 0
SMODE ?= 0
# 1: テスト終了後に QEMU を自動終了 / 0: 終了せず idle で待つ
TEST_EXIT ?= 1
# 構成 B-2: Sv39 MMU（S-mode のみ）。SMODE=1 のとき既定で有効
MMU ?= $(SMODE)
# 1: テストのあとシェルを起動してキーボードから使う（MMU=1 のときだけ）/ 0: テストだけで終わる
RUN_SH ?= $(MMU)
ifeq ($(MMU),1)
  ifneq ($(SMODE),1)
    $(error MMU=1 は SMODE=1 のときだけ使えます)
  endif
endif

# Official T-Kernel 2.0 (see docs/TKERNEL2_INTEGRATION.md)
# 既定: B-Free-master/Program/bfree_rv64 から見た archive/tkernel_2-master
# 指定（環境変数やコマンドライン）が無効なら、下の候補を順に探す
TKERNEL2_ROOT ?= ../../archive/tkernel_2-master
TKERNEL2_CANDIDATES := ../../archive/tkernel_2-master \
                       ../B-Free-master/archive/tkernel_2-master \
                       ../tkernel_2
ifeq ($(wildcard $(TKERNEL2_ROOT)/include/basic.h),)
  TKERNEL2_FOUND := $(firstword $(foreach d,$(TKERNEL2_CANDIDATES),$(if $(wildcard $(d)/include/basic.h),$(d))))
  ifneq ($(TKERNEL2_FOUND),)
    $(info NOTE: TKERNEL2_ROOT=$(TKERNEL2_ROOT) が無効なので $(TKERNEL2_FOUND) を使います)
    override TKERNEL2_ROOT := $(TKERNEL2_FOUND)
  endif
endif
TK_SRC_DIR := $(TKERNEL2_ROOT)/kernel/tkernel/src
TKERNEL2_STAMP := build/.tkernel2.ok

ifeq ($(TK2),1)
ifeq ($(SMODE),1)
  TARGET   := build/tk2-rv64-tk-smode.elf
  LDSCRIPT := kernel/sysdepend/rv64/Kernel_smode.ld
else
  TARGET   := build/tk2-rv64-tk.elf
  LDSCRIPT := kernel/sysdepend/rv64/Kernel.ld
endif
  OFFSET_H := kernel/sysdepend/cpu/rv64/offset.h

  TKERNEL_C := \
      tkstart.c timer.c wait.c task.c klock.c task_manage.c task_sync.c \
      mutex.c subsystem.c objname.c misc_calls.c version.c time_calls.c
  # bitsearch0.c / bitsearch1.c は LP64 で誤動作するため移植層の tk_port_bitsearch.c で置き換え
  LIBSYS_C := bitset.c bitclr.c bittest.c
  TKERNEL_OBJS := $(addprefix build/tk/,$(TKERNEL_C:.c=.o)) \
                  build/lib/libstr_string.o \
                  $(addprefix build/libsys/,$(LIBSYS_C:.c=.o))

  SRCS_S := \
      kernel/sysdepend/rv64/reset.S \
      kernel/sysdepend/cpu/rv64/exception.S \
      kernel/sysdepend/cpu/rv64/cpu_support_tk.S
ifneq ($(SMODE),1)
  SRCS_S += kernel/sysdepend/rv64/clint.S
endif

  SRCS_C := \
      kernel/tk/knl_main.c \
      kernel/tk/m5_tk_task.c \
      kernel/tk/m6_tick_test.c \
      kernel/tk/m7_preempt_test.c \
      kernel/sysdepend/cpu/rv64/cpu_init.c \
      kernel/sysdepend/cpu/rv64/kernel_exception.c \
      kernel/sysdepend/cpu/rv64/kernel_syscall.c \
      kernel/sysdepend/cpu/rv64/chkplv.c \
      kernel/sysdepend/cpu/rv64/cache.c \
      kernel/sysdepend/cpu/rv64/cpu_calls_port.c \
      kernel/sysdepend/cpu/rv64/cpu_dispatch_hook.c \
      kernel/sysdepend/cpu/rv64/m5b_trace.c \
      kernel/sysdepend/device/common/uart_virt.c \
      kernel/sysdepend/device/virt_rv64/devinit.c \
      kernel/sysdepend/device/virt_rv64/irq_handler.c \
      kernel/sysdepend/device/virt_rv64/virt_tk_timer.c \
      kernel/sysdepend/device/virt_rv64/poweroff.c \
      kernel/sysdepend/device/tef_virt_rv64/tkdev_init.c \
      kernel/sysdepend/device/tef_virt_rv64/patch.c \
      kernel/sysdepend/device/tef_virt_rv64/power.c \
      kernel/tk/port/tk_port_boot.c \
      kernel/tk/port/tk_port_sysconf.c \
      kernel/tk/port/tk_port_mem.c \
      kernel/tk/port/tk_port_string.c \
      kernel/tk/port/tk_port_tm.c \
      kernel/tk/port/tk_port_scinfo.c \
      kernel/tk/port/tk_port_api.c \
      kernel/tk/port/tk_port_stub.c \
      kernel/tk/port/tk_port_diag.c \
      kernel/tk/port/tk_port_kernel.c \
      kernel/tk/port/offset_check.c \
      kernel/tk/port/tk_port_bitsearch.c \
      kernel/sysdepend/cpu/rv64/fpu_ctx.c \
      kernel/sysdepend/device/virt_rv64/virtio_blk.c \
      kernel/fs/tarfs.c \
      kernel/tk/m13_disk_test.c
ifeq ($(MMU),1)
  SRCS_C += kernel/sysdepend/cpu/rv64/mmu_sv39.c kernel/tk/m8_mmu_test.c kernel/tk/m9_user_test.c kernel/tk/m10_multi_user_test.c kernel/tk/m11_uspace_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/uspace.c kernel/tk/m12_elf_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/usys_file.c kernel/tk/m14_usys_file_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/usys_console.c kernel/tk/m15_shell_test.c
  SRCS_C += kernel/sysdepend/device/virt_rv64/plic_uart.c kernel/tk/m16_uart_irq_test.c kernel/tk/m17_blk_irq_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/linux_syscall.c kernel/tk/m18_linux_hello_test.c kernel/tk/m19_fpvec_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/linux_fs.c kernel/sysdepend/cpu/rv64/linux_tmpfs.c kernel/tk/m20_linux_fs_test.c
  SRCS_C += kernel/sysdepend/cpu/rv64/linux_proc.c kernel/tk/m21_linux_proc_test.c kernel/tk/m22_busybox_test.c kernel/tk/m23_signal_test.c kernel/tk/m24_bash_test.c kernel/tk/m25_net_test.c kernel/tk/m26_ssh_test.c kernel/tk/m27_rt_test.c kernel/tk/m28_basic_test.c kernel/tk/m29_tmpfs_test.c kernel/tk/m30_usb_test.c kernel/tk/m31_usbkbd_test.c kernel/tk/m32_usbptr_test.c kernel/tk/m33_usbio_test.c kernel/tk/m34_som_test.c kernel/tk/m35_usbms_test.c kernel/tk/m36_data_test.c kernel/tk/m37_desktop_test.c kernel/tk/m38_wayland_base_test.c kernel/tk/m39_io_safety_test.c
  SRCS_C += kernel/sysdepend/device/virt_rv64/virtio_net.c kernel/sysdepend/device/virt_rv64/virtio_snd.c kernel/sysdepend/device/virt_rv64/virtio_gpu.c kernel/sysdepend/device/virt_rv64/pci_virt.c kernel/usb/xhci.c kernel/gui/desktop.c kernel/gui/font16.c kernel/fs/ext2.c kernel/io/iomgr.c kernel/io/firmata.c kernel/io/fdt.c kernel/io/som_pins.c kernel/net/net_ip.c kernel/net/net_tcp.c kernel/sysdepend/cpu/rv64/linux_net.c kernel/sysdepend/cpu/rv64/linux_pty.c kernel/sysdepend/cpu/rv64/linux_mm.c kernel/sysdepend/cpu/rv64/linux_unix.c kernel/sysdepend/cpu/rv64/linux_evfd.c
  SRCS_S += kernel/sysdepend/cpu/rv64/user_mode.S kernel/tk/user/user_prog.S kernel/tk/user/user_blobs.S
endif
ifeq ($(SMODE),1)
  SRCS_C += kernel/sysdepend/device/common/timer_sbi.c
else
  SRCS_C += kernel/sysdepend/device/common/timer_clint.c
endif

  OBJS := $(patsubst %.S, build/%.o, $(SRCS_S)) \
          $(patsubst %.c, build/%.o, $(SRCS_C)) \
          $(TKERNEL_OBJS)

  INCLUDES := -Iinclude \
              -Ikernel/sysdepend/cpu/rv64 \
              -Ikernel/sysdepend/device/virt_rv64 \
              -Ikernel/sysdepend/device/tef_virt_rv64 \
              -Ikernel/tk/port \
              -I$(TKERNEL2_ROOT)/include \
              -I$(TK_SRC_DIR)

  CFLAGS := $(BASE_CFLAGS) $(INCLUDES) \
            -include kernel/tk/port/config.h \
            -DM5B_TKERNEL -DTK2_TKERNEL -D_TEF_VIRT_RV64_ -DTEF_VIRT_RV64=1
ifeq ($(SMODE),1)
  CFLAGS += -DTK2_SMODE
endif
CFLAGS += -DTK2_TEST_EXIT=$(TEST_EXIT) -DTK2_MMU=$(MMU) -DTK2_RUN_SH=$(RUN_SH)
# ヘッダの依存関係を .d に書き出す（ヘッダを変えたら使っている所を作り直す）
CFLAGS += -MMD -MP

  AFLAGS := $(CFLAGS) -include $(OFFSET_H)
  LDFLAGS := $(ARCH) -nostdlib -T $(LDSCRIPT) -Wl,--gc-sections -Wl,-z,max-page-size=4096

else
  TARGET := tk2-rv64.elf
  LDSCRIPT := linker.ld
  ASRC := sysdepend/boot.S sysdepend/trap.S sysdepend/switch.S sysdepend/clint.S
  CSRC := src/main.c src/uart.c src/timer.c src/trap_handler.c \
          src/kernel/knl_main.c src/kernel/knl_test.c src/kernel/knl_sched.c
  OBJS := $(ASRC:.S=.o) $(CSRC:.c=.o)
  INCLUDES := -Iinclude
  CFLAGS := $(BASE_CFLAGS) -O2 $(INCLUDES)
  ASFLAGS := $(ARCH)
  LDFLAGS := $(ARCH) -nostdlib -T $(LDSCRIPT) -Wl,--gc-sections -Wl,-z,max-page-size=4096 -Wl,--no-relax
endif

.PHONY: all clean run qemu tk2-check clone-tkernel2 check-deps

ifeq ($(TK2),1)
all: check-deps $(OFFSET_H) $(TARGET) build/disk.img
else
all: $(TARGET)
endif

check-deps:
	@command -v $(CC) >/dev/null 2>&1 || { \
	  echo "ERROR: $(CC) not found. Install riscv64-unknown-elf-gcc."; exit 127; }
ifeq ($(TK2),1)
	@test -f "$(TKERNEL2_ROOT)/include/basic.h" || { \
	  echo "ERROR: tkernel_2 not found at TKERNEL2_ROOT=$(TKERNEL2_ROOT)"; exit 1; }
	@if { [ -f build/.smode ] && [ "$$(cat build/.smode)" != "$(SMODE)-$(TEST_EXIT)-$(MMU)-$(RUN_SH)" ]; } || \
	    { [ ! -f build/.smode ] && [ -d build/kernel ]; }; then \
	  echo "SMODE/TEST_EXIT/MMU/RUN_SH changed -> clean objects"; \
	  rm -rf build/kernel build/tk build/lib build/libsys build/offset_vals.s $(OFFSET_H); \
	fi
	@mkdir -p build
	@echo "$(SMODE)-$(TEST_EXIT)-$(MMU)-$(RUN_SH)" > build/.smode
	@echo "TKERNEL2_ROOT=$(TKERNEL2_ROOT)" > $(TKERNEL2_STAMP)
endif

tk2-check:
	@test -f "$(TKERNEL2_ROOT)/include/basic.h" || { \
	  echo "ERROR: tkernel_2 not found at TKERNEL2_ROOT=$(TKERNEL2_ROOT)"; exit 1; }
	@echo "OK: TKERNEL2_ROOT=$(TKERNEL2_ROOT)"
	@echo "Build: make TK2=1"

clone-tkernel2:
	git clone --depth 1 https://github.com/tron-forum/tkernel_2.git "$(TKERNEL2_ROOT)"

ifeq ($(TK2),1)
build/offset_vals.s: kernel/tk/port/offset_vals.c | build_dirs check-deps
	$(CC) $(CFLAGS) -std=gnu11 -S -o $@ $<

$(OFFSET_H): build/offset_vals.s
	@set -e; \
	off_dec() { \
	  v=$$(awk -v sym="off_export_$$1" ' \
	    $$0 ~ ("^[[:space:]]*" sym "[[:space:]]*:") { \
	      while (getline > 0) { \
	        if ($$1 == ".8byte" || $$1 == ".dword" || $$1 == ".quad") { print $$2; exit } \
	        if ($$0 ~ /^[^ \t#\.]/) exit \
	      } \
	    }' build/offset_vals.s); \
	  test -n "$$v" || { echo "ERROR: offset.h: missing off_export_$$1"; exit 1; }; \
	  echo "$$v"; \
	}; \
	tskid=$$(off_dec tskid); tskatr=$$(off_dec tskatr); state=$$(off_dec state); \
	reqdct=$$(off_dec reqdct); tskctxb=$$(off_dec tskctxb); \
	ctx_uatb=$$(off_dec CTXB_uatb); ctx_lsid=$$(off_dec CTXB_lsid); \
	ctx_svc_ssp=$$(off_dec CTXB_svc_ssp); \
	{ \
	  echo "/* AUTO-GENERATED - do not edit */"; \
	  echo "#ifndef _OFFSET_"; echo "#define _OFFSET_"; \
	  echo "#define TCB_tskid $$tskid"; \
	  echo "#define TCB_tskatr $$tskatr"; \
	  echo "#define TCB_state $$state"; \
	  echo "#define TCB_reqdct $$reqdct"; \
	  echo "#define TCB_tskctxb $$tskctxb"; \
	  echo "#define CTXB_ssp 0"; \
	  echo "#define CTXB_uatb $$ctx_uatb"; \
	  echo "#define CTXB_lsid $$ctx_lsid"; \
	  echo "#define CTXB_svc_ssp $$ctx_svc_ssp"; \
	  echo "#define TA_FPU 0"; \
	  echo "#endif"; \
	} > $@.tmp && mv -f $@.tmp $@; \
	echo "Generated $@ (TCB_tskctxb=$$tskctxb)"

build/kernel/sysdepend/cpu/rv64/cpu_support_tk.o: kernel/sysdepend/cpu/rv64/cpu_support_tk.S $(OFFSET_H) | build_dirs
	$(CC) $(AFLAGS) -c -o $@ $<

build/tk/%.o: $(TK_SRC_DIR)/%.c | build_dirs check-deps
	$(CC) $(CFLAGS) -std=gnu11 -c -o $@ $<
build/lib/libstr_string.o: $(TKERNEL2_ROOT)/lib/libstr/src/string.c | build_dirs check-deps
	$(CC) $(CFLAGS) -std=gnu11 -c -o $@ $<
build/libsys/%.o: $(TKERNEL2_ROOT)/lib/libsys/src/%.c | build_dirs check-deps
	$(CC) $(CFLAGS) -std=gnu11 -c -o $@ $<
endif

$(TARGET): $(OBJS) | build_dirs
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@
	@echo "Built: $@"

ifeq ($(TK2),1)
build/%.o: %.S | build_dirs
	$(CC) $(AFLAGS) -c -o $@ $<

# ユーザープログラム（ELF）: カーネルとは別にビルドし、user_blobs.S で埋め込む
#   U-mode では浮動小数点を使わない（rv64imac / lp64）
USER_CFLAGS := -march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding -fno-builtin \
               -fno-pic -fno-stack-protector -nostdlib -O1 -Wall -Wextra -Iuser/lib
# ユーザープログラムは user/名前/名前.c を 1 つずつ ELF にする（build/user/名前.elf）
USER_PROGS := hello fsdemo sh cat
USER_DEPS  := user/lib/crt0.S user/lib/usys.h user/lib/ulib.h user/user.ld
define USER_PROG_RULE
build/user/$(1).elf: user/$(1)/$(1).c $$(USER_DEPS)
	@mkdir -p build/user
	$$(CC) $$(USER_CFLAGS) -T user/user.ld -Wl,--no-relax -Wl,-z,max-page-size=4096 \
	  -o build/user/$(1).full.elf user/lib/crt0.S user/$(1)/$(1).c
	$$(OBJCOPY) --strip-all build/user/$(1).full.elf $$@
endef
$(foreach p,$(USER_PROGS),$(eval $(call USER_PROG_RULE,$(p))))
USER_ELFS := $(addprefix build/user/,$(addsuffix .elf,$(USER_PROGS)))

# RISC-V の Linux 用プログラム（musl で静的リンク）。Tamazusa-RV の Linux 互換層で動かす。
#   musl は tools/build_musl_rv64.sh で一度だけ用意する（toolchain/musl-rv64）。
#   見つからなければ作らない（M18 は SKIP と表示）。
MUSL_GCC ?= $(firstword $(wildcard toolchain/musl-rv64/bin/musl-gcc))
LINUX_PROGS := hello_musl fpvec_test fstest proctest sigtest nettest ptytest basic wltest iotest
ifneq ($(MUSL_GCC),)
LINUX_ELFS := $(addprefix build/user/,$(addsuffix .elf,$(LINUX_PROGS)))
# fpvec_test はベクトルのレジスタ名を asm で使うので V 拡張を明示する
LINUX_CFLAGS_fpvec_test := -march=rv64gcv -mabi=lp64d
build/user/basic.elf: user/linux/sjis_table.h include/vio_sim.h include/iomgr_core.h
build/user/%.elf: user/linux/%.c
	@mkdir -p build/user
	$(MUSL_GCC) -static -O2 -Wall $(LINUX_CFLAGS_$*) -o $@ $<
else
LINUX_ELFS :=
$(info NOTE: musl (toolchain/musl-rv64) がないので Linux 用プログラムは作りません。tools/build_musl_rv64.sh で用意できます)
endif

build/kernel/tk/user/user_blobs.o: build/user/hello.elf

# 仮想ディスク: tar（ustar）にユーザープログラムとテスト用のファイルを入れる
DISK_IMG := build/disk.img
# busybox（RISC-V の Linux 用、musl で静的リンク）。tools/build_busybox_rv64.sh で用意する
BUSYBOX_BIN := $(wildcard toolchain/busybox-rv64/busybox)
BUSYBOX_LINKS := $(wildcard toolchain/busybox-rv64/busybox.links)
BASH_BIN := $(wildcard toolchain/bash-rv64/bash)
DROPBEAR_BIN := $(wildcard toolchain/dropbear-rv64/dropbear)
DISK_EXTRA := $(if $(BUSYBOX_BIN),busybox.elf) $(if $(BUSYBOX_LINKS),busybox.links) $(if $(BASH_BIN),bash.elf) \
              $(if $(DROPBEAR_BIN),dropbear.elf dropbear.links)

$(DISK_IMG): $(USER_ELFS) $(LINUX_ELFS) user/disk/readme.txt user/disk/hello.sh user/disk/catself user/disk/sshd $(wildcard user/disk/etc/*) $(wildcard user/disk/bas/*) $(BUSYBOX_BIN) $(BUSYBOX_LINKS) $(BASH_BIN) $(DROPBEAR_BIN)
	rm -rf build/diskroot && mkdir -p build/diskroot
	cp $(USER_ELFS) $(LINUX_ELFS) user/disk/readme.txt user/disk/hello.sh user/disk/catself user/disk/sshd build/diskroot/
	chmod 755 build/diskroot/hello.sh build/diskroot/catself build/diskroot/sshd
	cp -r user/disk/etc build/diskroot/etc
	cp -r user/disk/bas build/diskroot/bas
	$(if $(BUSYBOX_BIN),cp $(BUSYBOX_BIN) build/diskroot/busybox.elf)
	$(if $(BUSYBOX_LINKS),cp $(BUSYBOX_LINKS) build/diskroot/busybox.links)
	$(if $(BASH_BIN),cp $(BASH_BIN) build/diskroot/bash.elf)
	$(if $(DROPBEAR_BIN),cp $(DROPBEAR_BIN) build/diskroot/dropbear.elf)
	$(if $(DROPBEAR_BIN),cp toolchain/dropbear-rv64/dropbear.links build/diskroot/)
	tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 \
	  -C build/diskroot -cf $@ $(notdir $(USER_ELFS) $(LINUX_ELFS)) readme.txt hello.sh catself sshd etc bas $(DISK_EXTRA)
build/%.o: %.c | build_dirs
	$(CC) $(CFLAGS) -std=gnu11 -c -o $@ $<
else
%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
endif

build_dirs:
	@mkdir -p $(sort $(dir $(OBJS))) build/tk build/lib build/libsys

QEMU_BIN  := qemu-system-riscv64
ifeq ($(SMODE),1)
QEMU_BIOS := default
else
QEMU_BIOS := none
endif
# CPU: RVA23（Ubuntu 25.10 以降の RISC-V 用コンパイラが前提にする世代。K3 も RVA23）
#   QEMU 10 以降は rva23s64。古い QEMU では、主な拡張命令を一つずつ有効にした rv64 で代わりにする
QEMU_HAS_RVA23 := $(shell $(QEMU_BIN) -cpu help 2>/dev/null | grep -q rva23s64 && echo 1)
ifeq ($(QEMU_HAS_RVA23),1)
QEMU_CPU ?= rva23s64
else
QEMU_CPU ?= rv64,v=true,vlen=128,zba=true,zbb=true,zbs=true,zbc=true,zcb=true,zicond=true,zfa=true,zfh=true,zfhmin=true,zicbom=true,zicboz=true,zihintntl=true,zihintpause=true,zawrs=true,zvbb=true,zkt=true
endif
QEMU_BASE := -machine virt,acpi=off -cpu $(QEMU_CPU) -m 128M -display none \
             -serial mon:stdio -bios $(QEMU_BIOS) -kernel $(TARGET)
ifeq ($(TK2),1)
  # 仮想ディスク（virtio-blk、virtio-mmio 版 2）
  QEMU_BASE += -global virtio-mmio.force-legacy=false \
               -drive file=build/disk.img,if=none,format=raw,id=hd0,readonly=on \
               -device virtio-blk-device,drive=hd0
  # 仮想 LAN（virtio-net、QEMU のユーザーモード: 10.0.2.0/24）。
  # パソコンの 2222 番へつなぐと Tamazusa の 22 番（sshd）、8080 番は 80 番へ（パソコンの中からだけ）
  QEMU_NET ?= user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:80
  QEMU_BASE += -netdev $(QEMU_NET) -device virtio-net-device,netdev=n0
  # 音（virtio-sound）。パソコンの音の仕組みを pa（WSLg・PulseAudio）→ pipewire の順に探し、
  # なければ none（鳴らないが、装置は時間どおりに音を受け取る）。QEMU_AUDIO=none で消せる
  QEMU_HAS_SND := $(shell $(QEMU_BIN) -device help 2>/dev/null | grep -q '"virtio-sound-device"' && echo 1)
  # pa は、pactl があれば 3 秒で応答を確かめる（WSLg の音の窓口が止まっていると QEMU が起動しないため）。
  # 応答しなければ none（音なし）で起動する。pactl がなければ確かめずに pa を使う
  QEMU_AUDIO ?= $(or $(shell \
      if $(QEMU_BIN) -audiodev help 2>/dev/null | grep -qx pa; then \
        if command -v pactl >/dev/null 2>&1; then \
          timeout 3 pactl info >/dev/null 2>&1 && echo pa; \
        else echo pa; fi; \
      elif $(QEMU_BIN) -audiodev help 2>/dev/null | grep -qx pipewire; then echo pipewire; fi),none)
  ifeq ($(QEMU_HAS_SND),1)
    QEMU_BASE += -audiodev $(QEMU_AUDIO),id=snd0 -device virtio-sound-device,audiodev=snd0
  endif
  # 画面（virtio-gpu、1024×768 のデスクトップ。/dev/fb0 は 640×400）。パソコンに窓が出せれば gtk → sdl の順に使う（WSLg なら窓が出る）。
  # 窓が要らなければ QEMU_DISPLAY=none
  QEMU_DISPLAY ?= $(or $(shell for d in gtk sdl; do $(QEMU_BIN) -display help 2>/dev/null | grep -qx $$d && echo $$d && break; done),none)
  QEMU_BASE := $(filter-out -display none,$(QEMU_BASE))
  QEMU_BASE += -device virtio-gpu-device -display $(QEMU_DISPLAY)
  # USB（xHCI、PCIe）。キーボードとタブレット（マウス）を 1 台ずつ
  QEMU_BASE += -device qemu-xhci,id=xhci,p2=8,p3=4 -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
  # 書き込めるディスク（/data）: 64MiB の ext2（なければ mke2fs で作る。消すと空に戻る）
  QEMU_BASE += -drive file=build/data.img,if=none,format=raw,id=data0 -device virtio-blk-device,drive=data0
  # USB メモリ: 16MiB のイメージ（なければ作る。頭に目印の文字）
  QEMU_BASE += -drive file=build/usb.img,if=none,format=raw,id=usb0 -device usb-storage,bus=xhci.0,drive=usb0
endif

# USB の I/O ボード（Firmata）の模擬: python3 があれば、tools/firmata_sim.py を QEMU の usb-serial につなぐ。
# 本物のボードはないので、ボードの代わりに Firmata で答える。USBIO=0 で消せる
USBIO ?= $(if $(shell command -v python3 2>/dev/null),1,0)
# ソケットは Linux 側の /tmp に置く（WSL の /mnt/c のような Windows のフォルダには、UNIX ソケットを作れない）
FIRMATA_SOCK ?= /tmp/tamazusa-firmata-$(shell id -u).sock
ifeq ($(USBIO),1)
  USBIO_ARGS := -chardev socket,id=fm0,path=$(FIRMATA_SOCK),server=on,wait=off -device usb-serial,chardev=fm0,bus=xhci.0
endif

build/data.img:
	@mkdir -p build
	truncate -s 64M $@
	PATH="$$PATH:/sbin:/usr/sbin" mke2fs -q -F -t ext2 -b 4096 -I 256 -O ^dir_index -L tamazusa-data $@

build/usb.img:
	@mkdir -p build
	truncate -s 16M $@
	printf 'TAMAZUSA USB MEMORY TEST IMAGE\n' | dd of=$@ conv=notrunc status=none

qemu: all fix-crlf build/usb.img build/data.img
	@if [ "$(USBIO)" = "1" ]; then rm -f $(FIRMATA_SOCK); python3 tools/firmata_sim.py $(FIRMATA_SOCK) > build/firmata_sim.log 2>&1 & fi
	$(QEMU_BIN) $(QEMU_BASE) $(USBIO_ARGS) $(QEMU_EXTRA)

run: qemu

fix-crlf:
	sed -i 's/\r$$//' run_qemu.sh 2>/dev/null || true
	chmod +x run_qemu.sh 2>/dev/null || true

ifeq ($(TK2),1)
-include $(OBJS:.o=.d)
endif

clean:
	rm -rf build tk2-rv64.elf
	@rm -f kernel/sysdepend/cpu/rv64/offset.h build/offset_vals.s $(TKERNEL2_STAMP) 2>/dev/null || true
