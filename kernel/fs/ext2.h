/* kernel/fs/ext2.h — ext2 のファイルシステム（読み書き） */
#ifndef KERNEL_FS_EXT2_H
#define KERNEL_FS_EXT2_H

#include <stdint.h>
#include <stddef.h>

#define EXT2_ROOT_INO   2
#define EXT2_PATH_MAX   256
#define EXT2_REDIRECT   (-1000)     /* シンボリックリンク: redir の新しいパスでたどり直す */
#define EXT2_CREAT      1
#define EXT2_EXCL       2
#define EXT2_TRUNC      4

struct ext2_stat {
    uint32_t ino;
    uint16_t mode, nlink, uid, gid;
    uint64_t size;
    uint32_t blocks;            /* 512 バイトの数 */
    uint32_t atime, mtime, ctime;
    uint32_t blksize;
};

/* 失敗は負の errno。redir は EXT2_PATH_MAX バイト（EXT2_REDIRECT のとき、新しいパスが入る） */
int  ext2_mount(int (*rd)(uint64_t, void *, unsigned), int (*wr)(uint64_t, const void *, unsigned),
                uint64_t nsectors, uint32_t (*clock)(void));
int  ext2_mounted(void);
int  ext2_sync(void);           /* ためてあるものをディスクへ（fsync・sync・見回り・電源を切る前） */
int  ext2_dirty_blocks(void);
void ext2_start_sync_task(void);  /* 1 秒ごとの見回り（カーネルだけ） */
int  ext2_readonly(void);
void ext2_set_prefix(const char *p);
int  ext2_lookup(const char *path, int follow, uint32_t *ino, char *redir);
int  ext2_stat(uint32_t ino, struct ext2_stat *st);
long ext2_read(uint32_t ino, uint64_t off, void *buf, size_t len);
long ext2_write(uint32_t ino, uint64_t off, const void *buf, size_t len);
int  ext2_truncate(uint32_t ino, uint64_t len);
int  ext2_open(const char *path, int flags, uint16_t perm, uint32_t *ino, char *redir);
int  ext2_mkdir(const char *path, uint16_t perm, char *redir);
int  ext2_symlink(const char *target, const char *path, char *redir);
long ext2_readlink(const char *path, char *buf, size_t max, char *redir);
int  ext2_remove(const char *path, int dir, char *redir);
int  ext2_rename(const char *from, const char *to, char *redir);
int  ext2_link(const char *from, const char *to, char *redir);
int  ext2_chmod(uint32_t ino, uint16_t perm);
int  ext2_utime(uint32_t ino, uint32_t atime, uint32_t mtime);
int  ext2_readdir(uint32_t ino, uint64_t *pos, char *name, size_t max, uint32_t *child, uint8_t *type);
void ext2_iref(uint32_t ino);
void ext2_iunref(uint32_t ino);
void ext2_statfs(uint64_t *bsize, uint64_t *blocks, uint64_t *bfree, uint64_t *files, uint64_t *ffree);

#endif
