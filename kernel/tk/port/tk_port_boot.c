/* tk_port_boot.c — M5b: board_init → t_kernel_main (official TK2) */

#include "knlconfig.h"
#include <tk/tkernel.h>
#include <tk/typedef.h>
#include <tm/tmonitor.h>
#if !defined(__aarch64__)
#include "rv_priv.h"
#endif

extern void uart_puts(const char *s);
extern void m5b_t_kernel_main(T_CTSK *inittask);
extern ER init_Imalloc(void);

static void irq_mask_all(void)
{
#if defined(__aarch64__)
    asm volatile("msr daifset, #2" ::: "memory");
#else
    asm volatile("csrc " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE));
    asm volatile("csrc " RV_STR(CSR_XIE) ", %0" :: "r"((unsigned long)XIE_TIMER));
#endif
}

static void m5b_init_task(void);

static const T_CTSK c_m5b_init = {
    .tskatr = TA_HLNG | TA_RNG0,
    .task = (FP)m5b_init_task,
    .itskpri = 12,
    .stksz = 8192,
    .sstksz = 0,
    .stkptr = 0,
    .uatb = 0,
    .lsid = 0,
    .resid = 0,
};

static void m5b_init_task(void)
{
    extern int m5_run_tk_task(void);
    extern int m6_run_tick_test(void);
    extern int m7_run_preempt_test(void);
    int pass;

    uart_puts("[M5b] init_task running\r\n");
    pass = m5_run_tk_task();
    pass = m6_run_tick_test() && pass;
    pass = m7_run_preempt_test() && pass;
#if TK2_MMU
    {
        extern int m8_run_mmu_test(void);
        extern int m9_run_user_test(void);
        extern int m10_run_multi_user_test(void);
        extern int m11_run_uspace_test(void);
        pass = m8_run_mmu_test() && pass;
        pass = m9_run_user_test() && pass;
        pass = m10_run_multi_user_test() && pass;
        pass = m11_run_uspace_test() && pass;
#if !defined(TK2_NO_ELF)
        {
            extern int m12_run_elf_test(void);
            pass = m12_run_elf_test() && pass;
        }
#endif
    }
#endif
#if !defined(TK2_NO_DISK)
    {
        extern int m13_run_disk_test(void);
        pass = m13_run_disk_test() && pass;
    }
#endif
#if TK2_MMU && !defined(TK2_CORE_ONLY)   /* TK2_CORE_ONLY: 装置・Linux を入れない小さな構成（いまは使っていない） */
    {
        extern void net_init(void);
        extern int virtio_snd_init(void);
        extern void rv_data_mount(void);
        rv_data_mount();                /* 2 台目のディスク（ext2）を /data に */
        extern void iomgr_init(void);
        iomgr_init();                   /* 入出力の管理（/dev/io・/etc/io.conf） */
        net_init();                     /* ネットワーク（virtio-net・IPv4・TCP） */
        extern int virtio_gpu_init(void);
        uart_puts(virtio_snd_init() ? "[snd] virtio-sound: /dev/dsp (22050Hz, 16bit, mono)\r\n"
                                    : "[snd] no sound device\r\n");
        uart_puts(virtio_gpu_init() ? "[gpu] virtio-gpu: desktop 1024x768, /dev/fb0 640x400 in a window\r\n"
                                    : "[gpu] no display device\r\n");
        {
            extern int xhci_init(void);
            extern int usb_hid_start(void);
            if (xhci_init() < 0) {
                uart_puts("[usb] no xHCI\r\n");
            } else {
                usb_hid_start();
            }
        }
    }
    {
        extern int m14_run_usys_file_test(void);
        extern int m15_run_shell_test(void);
        pass = m14_run_usys_file_test() && pass;
        pass = m15_run_shell_test() && pass;
        {
            extern int m16_run_uart_irq_test(void);
            extern int m17_run_blk_irq_test(void);
            pass = m16_run_uart_irq_test() && pass;
            extern int m18_run_linux_hello_test(void);
            pass = m17_run_blk_irq_test() && pass;
            extern int m19_run_fpvec_test(void);
            pass = m18_run_linux_hello_test() && pass;
            extern int m20_run_linux_fs_test(void);
            pass = m19_run_fpvec_test() && pass;
            extern int m21_run_linux_proc_test(void);
            pass = m20_run_linux_fs_test() && pass;
            extern int m22_run_busybox_test(void);
            pass = m21_run_linux_proc_test() && pass;
            pass = m22_run_busybox_test() && pass;
            {
                extern int m23_run_signal_test(void);
                extern int m24_run_bash_test(void);
                pass = m23_run_signal_test() && pass;
                pass = m24_run_bash_test() && pass;
                {
                    extern int m25_run_net_test(void);
                    extern int m26_run_ssh_test(void);
                    pass = m25_run_net_test() && pass;
                    pass = m26_run_ssh_test() && pass;
                    {
                        extern int m28_run_basic_test(void);
                        extern int m29_run_tmpfs_test(void);
                        extern int m30_run_usb_test(void);
                        extern int m31_run_usbkbd_test(void);
                        extern int m32_run_usbptr_test(void);
                        pass = m30_run_usb_test() && pass;
                        pass = m31_run_usbkbd_test() && pass;
                        pass = m32_run_usbptr_test() && pass;
                        extern int m33_run_usbio_test(void);
                        pass = m33_run_usbio_test() && pass;
                        extern int m34_run_som_test(void);
                        pass = m34_run_som_test() && pass;
                        extern int m35_run_usbms_test(void);
                        pass = m35_run_usbms_test() && pass;
                        extern int m36_run_data_test(void);
                        pass = m36_run_data_test() && pass;
                        extern int m37_run_desktop_test(void);
                        pass = m37_run_desktop_test() && pass;
                        extern int m38_run_wayland_base_test(void);
                        pass = m38_run_wayland_base_test() && pass;
                        extern int m39_run_io_safety_test(void);
                        pass = m39_run_io_safety_test() && pass;
                        pass = m29_run_tmpfs_test() && pass;
                        pass = m28_run_basic_test() && pass;
                    }
                    {
                        extern int m27_run_rt_test(void);
                        pass = m27_run_rt_test() && pass;
                    }
                }
            }
        }
    }
#endif
    uart_puts(pass ? "[TEST] ALL PASS\r\n" : "[TEST] FAILED\r\n");
#if TK2_MMU && TK2_RUN_SH
    {
        extern void rv_run_shell(void);
        {
            extern void desktop_raise(int which);
            desktop_raise(0);   /* 端末の窓を上に（テストの絵の窓に隠れないように） */
        }
        rv_run_shell();     /* キーボードから使う（exit で戻る） */
    }
#endif
#if TK2_MMU && !defined(TK2_CORE_ONLY)   /* TK2_CORE_ONLY: 装置・Linux を入れない小さな構成（いまは使っていない） */
    {
        extern int ext2_sync(void);
        ext2_sync();        /* /data のためてある分をディスクへ（電源を切る前） */
    }
#endif
#if TK2_TEST_EXIT
    {
        extern void rv_poweroff(int pass);
        rv_poweroff(pass);  /* QEMU を終了（戻らない） */
    }
#endif
    uart_puts("[M5b] init_task done\r\n");
    tk_ext_tsk();
}

void tk_port_boot(void)
{
    uart_puts("M5b tk_port_boot\r\n");
    /* board_init は reset.S で済み。二重 MMU/GIC 初期化を避ける */
    irq_mask_all();
    (void)init_Imalloc();
    uart_puts("[M5b] calling m5b_t_kernel_main\r\n");
    m5b_t_kernel_main((T_CTSK *)&c_m5b_init);
    uart_puts("[M5b] m5b_t_kernel_main returned?\r\n");
    for (;;) {
        asm volatile("wfi");
    }
}

void DispProgress(W n)
{
    (void)n;
}
