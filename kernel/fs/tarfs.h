/* tarfs.h — ディスク上の tar（ustar）を読む */
#ifndef TARFS_H
#define TARFS_H
#include <stdint.h>

struct tarfs_file {
    char     name[101];
    uint64_t start;     /* 中身の開始セクタ */
    uint64_t size;      /* バイト数 */
    uint32_t mode;      /* tar に記録された許可（0755 など） */
};

int     tarfs_entry(int index, struct tarfs_file *f);
int     tarfs_find(const char *name, struct tarfs_file *f);
int64_t tarfs_read(const struct tarfs_file *f, uint64_t offset, void *buf, uint64_t len);
int64_t tarfs_read_all(const struct tarfs_file *f, void *buf, uint64_t max);

int      virtio_blk_init(void);
uint64_t virtio_blk_capacity(void);
int      virtio_blk_read(uint64_t sector, void *buf, unsigned count);
int      virtio_blk_read_nocache(uint64_t sector, void *buf, unsigned count);   /* 装置へ必ず要求を出す（テスト用） */

#endif
