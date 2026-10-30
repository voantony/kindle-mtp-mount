/* minimal stand-in for <fuse.h> so the ops can be unit-tested without FUSE */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
struct fuse_file_info { int flags; uint64_t fh; };
struct fuse_conn_info { int unused; };
struct fuse_config { double entry_timeout, attr_timeout, negative_timeout; int hard_remove; };
enum fuse_readdir_flags { FUSE_READDIR_PLUS = 1 };
enum fuse_fill_dir_flags { FUSE_FILL_DIR_PLUS = 2 };
typedef int (*fuse_fill_dir_t)(void *, const char *, const struct stat *, off_t, enum fuse_fill_dir_flags);
struct fuse_operations {
  int (*getattr)(const char *, struct stat *, struct fuse_file_info *);
  int (*mkdir)(const char *, mode_t); int (*unlink)(const char *); int (*rmdir)(const char *);
  int (*rename)(const char *, const char *, unsigned int);
  int (*chmod)(const char *, mode_t, struct fuse_file_info *);
  int (*chown)(const char *, uid_t, gid_t, struct fuse_file_info *);
  int (*truncate)(const char *, off_t, struct fuse_file_info *);
  int (*open)(const char *, struct fuse_file_info *);
  int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
  int (*write)(const char *, const char *, size_t, off_t, struct fuse_file_info *);
  int (*statfs)(const char *, struct statvfs *);
  int (*flush)(const char *, struct fuse_file_info *);
  int (*release)(const char *, struct fuse_file_info *);
  int (*fsync)(const char *, int, struct fuse_file_info *);
  int (*setxattr)(const char *, const char *, const char *, size_t, int);
  int (*getxattr)(const char *, const char *, char *, size_t);
  int (*listxattr)(const char *, char *, size_t);
  int (*removexattr)(const char *, const char *);
  int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, enum fuse_readdir_flags);
  void *(*init)(struct fuse_conn_info *, struct fuse_config *);
  int (*create)(const char *, mode_t, struct fuse_file_info *);
  int (*utimens)(const char *, const struct timespec tv[2], struct fuse_file_info *);
};
#define fuse_main(argc, argv, ops, ud) (0)
