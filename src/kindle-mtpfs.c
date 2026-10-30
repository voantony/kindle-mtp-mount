/* kindle-mtpfs - show a Kindle (MTP over USB) as a Finder drive via macFUSE.
 * MIT License, see LICENSE. Links dynamically against libmtp (LGPL) and
 * macFUSE's libfuse3.
 *
 *   kindle-mtpfs [--ro] [--debug] /Volumes/Kindle
 *
 * Read-write by default; --ro makes every change return EROFS.
 * Deletes are immediate and permanent (there is no Trash on the Kindle).
 * macOS clutter (.DS_Store, ._* AppleDouble, .Trashes, .fseventsd, ...) is
 * never written to the device: those names live only in memory.
 * Never resets USB on close. If the Kindle is busy (Calibre, kpush, another
 * MTP app), it exits with an error instead of retrying.
 */
#define FUSE_USE_VERSION 31
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#include <fuse.h>
#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <pthread.h>
#include "redact.h"

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

/* ---------------------------------------------------------------- device */
/* Mirror of libmtp 1.1.23's internal struct _PTP_USB (libusb1 build) so the
 * reset-on-close flag libmtp re-adds for this device can be cleared. Same
 * approach as kpush.v5/v6; only valid for 1.1.23, checked at startup. */
typedef struct {
  void *params; void *handle;
  uint8_t config, interface, altsetting;
  int inep, inep_maxpacket, outep, outep_maxpacket, intep;
  int callback_active, timeout;
  uint16_t bcdusb;
  uint64_t current_transfer_total, current_transfer_complete;
  LIBMTP_progressfunc_t current_transfer_callback;
  void const *current_transfer_callback_data;
  LIBMTP_raw_device_t rawdevice;
} km_ptp_usb_mirror;
#define KINDLE_VID 0x1949
#define COLORSOFT1_PID 0x9981
#define KP_RESET_FLAG ((uint32_t)0x10000000) /* DEVICE_FLAG_FORCE_RESET_ON_CLOSE */
#define ROOT LIBMTP_FILES_AND_FOLDERS_ROOT

static LIBMTP_mtpdevice_t *dev;
static uint32_t storage_id;
static int opt_ro = 0, opt_debug = 0;
static volatile int mounted = 0; /* set once macFUSE calls init */
static char tmpdir[1024];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER; /* libmtp is not thread-safe */
#define LOCK() pthread_mutex_lock(&mtx)
#define UNLOCK() pthread_mutex_unlock(&mtx)
#define DBG(...) do { if (opt_debug) { fprintf(stderr, __VA_ARGS__); } } while (0)

static int no_reset_on_close(LIBMTP_mtpdevice_t *d) {
  km_ptp_usb_mirror *u = (km_ptp_usb_mirror *)d->usbinfo;
  if (u->rawdevice.device_entry.vendor_id != KINDLE_VID || u->rawdevice.device_entry.product_id != COLORSOFT1_PID) {
    fprintf(stderr, "layout check failed, not touching device\n"); return -1; }
  u->rawdevice.device_entry.device_flags &= ~KP_RESET_FLAG;
  return (u->rawdevice.device_entry.device_flags & KP_RESET_FLAG) ? -1 : 0;
}
static void mtp_err(const char *what) {
  fprintf(stderr, "MTP error: %s\n", what);
  LIBMTP_Dump_Errorstack(dev); LIBMTP_Clear_Errorstack(dev);
}
/* parent id as libmtp wants it for create/send: 0 means the root */
static uint32_t pid_for_create(uint32_t parent) { return parent == ROOT ? 0 : parent; }

/* ------------------------------------------------------- directory cache */
typedef struct { char *name; uint32_t id; int isdir; uint64_t size; time_t mtime; } ent_t;
typedef struct dc { char *path; uint32_t id; ent_t *e; int n; time_t at; struct dc *next; } dc_t;
static dc_t *dcache;
#define DIR_TTL 5

static void dc_free(dc_t *c) { for (int i = 0; i < c->n; i++) free(c->e[i].name); free(c->e); free(c->path); free(c); }
/* drop the cache for path and, when prefix is set, everything below it */
static void invalidate(const char *path, int prefix) {
  size_t L = strlen(path);
  for (dc_t **pp = &dcache; *pp;) {
    dc_t *c = *pp;
    int hit = !strcmp(c->path, path) ||
      (prefix && !strncmp(c->path, path, L) && (c->path[L] == '/' || L == 1));
    if (hit) { *pp = c->next; dc_free(c); } else pp = &c->next;
  }
}
static void parent_of(const char *path, char *out, size_t on) {
  snprintf(out, on, "%s", path);
  char *s = strrchr(out, '/');
  if (!s || s == out) snprintf(out, on, "/"); else *s = 0;
}
static const char *base_of(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }

static int lookup(const char *path, ent_t *out);
/* listing of the folder at path (cached for DIR_TTL seconds) */
static dc_t *get_dir(const char *path) {
  time_t now = time(NULL);
  for (dc_t **pp = &dcache; *pp; pp = &(*pp)->next) {
    dc_t *c = *pp;
    if (!strcmp(c->path, path)) {
      if (now - c->at < DIR_TTL) return c;
      *pp = c->next; dc_free(c); break;
    }
  }
  uint32_t id;
  if (!strcmp(path, "/")) id = ROOT;
  else { ent_t e; if (lookup(path, &e) || !e.isdir) return NULL; id = e.id; }
  dc_t *c = calloc(1, sizeof *c);
  c->path = strdup(path); c->id = id; c->at = now;
  LIBMTP_file_t *ls = LIBMTP_Get_Files_And_Folders(dev, storage_id, id);
  int n = 0; for (LIBMTP_file_t *f = ls; f; f = f->next) n++;
  c->e = calloc(n ? n : 1, sizeof *c->e);
  for (LIBMTP_file_t *f = ls; f; f = f->next) {
    if (!f->filename || !*f->filename || strchr(f->filename, '/') || !strcmp(f->filename, ".") || !strcmp(f->filename, "..")) continue;
    ent_t *e = &c->e[c->n++];
    e->name = strdup(f->filename); e->id = f->item_id; e->isdir = f->filetype == LIBMTP_FILETYPE_FOLDER;
    e->size = f->filesize; e->mtime = f->modificationdate;
  }
  while (ls) { LIBMTP_file_t *nx = ls->next; LIBMTP_destroy_file_t(ls); ls = nx; }
  LIBMTP_Clear_Errorstack(dev);
  c->next = dcache; dcache = c;
  DBG("listed %s: %d entries\n", path, c->n);
  return c;
}
static int lookup(const char *path, ent_t *out) {
  if (!strcmp(path, "/")) { memset(out, 0, sizeof *out); out->id = ROOT; out->isdir = 1; return 0; }
  char par[4096]; parent_of(path, par, sizeof par);
  dc_t *c = get_dir(par); if (!c) return -ENOENT;
  const char *nm = base_of(path);
  int found = -1;
  for (int i = 0; i < c->n; i++) if (!strcmp(c->e[i].name, nm)) { if (found < 0 || (c->e[i].isdir && !c->e[found].isdir)) found = i; }
  if (found < 0) return -ENOENT;
  *out = c->e[found]; return 0;
}
static uint32_t dir_id(const char *path) { ent_t e; if (lookup(path, &e) || !e.isdir) return 0; return e.id; }

/* -------------------------------------------------- macOS clutter names */
static int is_junk(const char *path) {
  const char *n = base_of(path);
  static const char *junk[] = { ".DS_Store", ".Trashes", ".Trash", ".Spotlight-V100", ".fseventsd",
    ".TemporaryItems", ".VolumeIcon.icns", ".apdisk", ".com.apple.timemachine.donotpresent",
    ".metadata_never_index", ".metadata_never_index_unless_rootfs", ".hidden", NULL };
  if (!strncmp(n, "._", 2)) return 1;
  for (int i = 0; junk[i]; i++) if (!strcmp(n, junk[i])) return 1;
  return 0;
}
/* junk files exist only in memory so Finder is happy, never on the device */
typedef struct ph { char *path; off_t size; struct ph *next; } ph_t;
static ph_t *phantoms;
static ph_t *ph_find(const char *p) { for (ph_t *x = phantoms; x; x = x->next) if (!strcmp(x->path, p)) return x; return NULL; }
static ph_t *ph_add(const char *p) { ph_t *x = ph_find(p); if (x) return x; x = calloc(1, sizeof *x); x->path = strdup(p); x->next = phantoms; phantoms = x; return x; }
static void ph_del(const char *p) { for (ph_t **pp = &phantoms; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->path, p)) { ph_t *x = *pp; *pp = x->next; free(x->path); free(x); return; } }

/* ----------------------------------------------------------- open files */
typedef struct fh {
  char path[4096];
  uint32_t id;          /* 0 while a new file has not been uploaded yet */
  uint64_t size;        /* size on the device when opened */
  int junk;             /* phantom: data discarded */
  int fd;               /* local temp copy, -1 if none yet */
  char tmp[1100];
  int dirty, writable, reads;
  struct fh *next;
} fh_t;
static fh_t *open_files;
static int no_partial = 0;

static fh_t *fh_new(const char *path) {
  fh_t *h = calloc(1, sizeof *h); snprintf(h->path, sizeof h->path, "%s", path); h->fd = -1;
  h->next = open_files; open_files = h; return h;
}
static void fh_drop(fh_t *h) {
  for (fh_t **pp = &open_files; *pp; pp = &(*pp)->next) if (*pp == h) { *pp = h->next; break; }
  if (h->fd >= 0) { close(h->fd); unlink(h->tmp); }
  free(h);
}
/* an open, not-yet-uploaded (or modified) file at path, if any */
static fh_t *fh_pending(const char *path) {
  for (fh_t *h = open_files; h; h = h->next) if (!h->junk && h->fd >= 0 && (h->dirty || !h->id) && !strcmp(h->path, path)) return h;
  return NULL;
}
static int fh_mktemp(fh_t *h) {
  static unsigned seq = 0;
  snprintf(h->tmp, sizeof h->tmp, "%s/f%u", tmpdir, ++seq);
  h->fd = open(h->tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
  return h->fd < 0 ? -errno : 0;
}
/* download the whole object into the handle's temp file */
static int fh_fetch(fh_t *h) {
  if (h->fd >= 0) return 0;
  int r = fh_mktemp(h); if (r) return r;
  if (!h->id) return 0;
  if (LIBMTP_Get_File_To_File_Descriptor(dev, h->id, h->fd, NULL, NULL)) {
    mtp_err("download failed"); close(h->fd); unlink(h->tmp); h->fd = -1; return -EIO; }
  struct stat st; fstat(h->fd, &st);
  DBG("fetched %s (%lld bytes)\n", h->path, (long long)st.st_size);
  if ((uint64_t)st.st_size != h->size) fprintf(stderr, "warning: %s downloaded %lld bytes, device said %llu\n", h->path, (long long)st.st_size, (unsigned long long)h->size);
  return 0;
}
/* fresh size of a file named nm in folder parent; returns count of matches */
static int find_named(uint32_t parent, const char *nm, uint32_t *id, uint64_t *size) {
  int c = 0; LIBMTP_file_t *ls = LIBMTP_Get_Files_And_Folders(dev, storage_id, parent);
  for (LIBMTP_file_t *f = ls; f; f = f->next)
    if (f->filetype != LIBMTP_FILETYPE_FOLDER && f->filename && !strcmp(f->filename, nm)) { c++; if (id) *id = f->item_id; if (size) *size = f->filesize; }
  while (ls) { LIBMTP_file_t *nx = ls->next; LIBMTP_destroy_file_t(ls); ls = nx; }
  return c;
}
static int safe_delete(uint32_t id, uint32_t parent) {
  if (id == 0 || id == 0xFFFFFFFFu || id == ROOT || id == parent) { fprintf(stderr, "REFUSING delete of object id 0x%08x\n", id); return -EPERM; }
  return LIBMTP_Delete_Object(dev, id) ? -EIO : 0;
}
static int send_fd(int fd, uint32_t parent, const char *name, uint64_t size, uint32_t *newid) {
  lseek(fd, 0, SEEK_SET);
  LIBMTP_file_t *nf = LIBMTP_new_file_t();
  nf->filename = strdup(name); nf->filesize = size; nf->filetype = LIBMTP_FILETYPE_UNKNOWN;
  nf->parent_id = pid_for_create(parent); nf->storage_id = storage_id;
  int r = LIBMTP_Send_File_From_File_Descriptor(dev, fd, nf, NULL, NULL);
  if (!r && newid) *newid = nf->item_id;
  LIBMTP_destroy_file_t(nf);
  if (r) { mtp_err("upload failed"); return -EIO; }
  return 0;
}
/* Upload the temp file: new name -> upload and verify size. Existing name ->
 * upload under name.kmtmp, verify, delete the old object, rename the temp. */
static int fh_commit(fh_t *h) {
  if (h->junk || !h->dirty || h->fd < 0) return 0;
  struct stat st; if (fstat(h->fd, &st)) return -errno;
  uint64_t want = (uint64_t)st.st_size;
  char par[4096]; parent_of(h->path, par, sizeof par);
  const char *nm = base_of(h->path);
  uint32_t parent = dir_id(par); if (!parent) return -ENOENT;
  uint32_t old_id = 0, nid = 0; uint64_t got = 0;
  int n = find_named(parent, nm, &old_id, NULL);
  if (n > 1) { fprintf(stderr, "%s: %d copies on device, not replacing\n", h->path, n); return -EIO; }
  int r;
  if (n == 0) {
    if ((r = send_fd(h->fd, parent, nm, want, &nid))) return r;
    if (find_named(parent, nm, &nid, &got) != 1 || got != want) { fprintf(stderr, "%s: verify failed (device %llu, want %llu)\n", h->path, (unsigned long long)got, (unsigned long long)want); invalidate(par, 0); return -EIO; }
  } else {
    char tn[1024]; snprintf(tn, sizeof tn, "%s.kmtmp", nm);
    if (find_named(parent, tn, NULL, NULL)) { fprintf(stderr, "%s: temp name %s already exists\n", h->path, tn); return -EEXIST; }
    uint32_t tid = 0;
    if ((r = send_fd(h->fd, parent, tn, want, &tid))) return r;
    if (find_named(parent, tn, &tid, &got) != 1 || got != want) { fprintf(stderr, "%s: temp verify failed, old file untouched\n", h->path); invalidate(par, 0); return -EIO; }
    if ((r = safe_delete(old_id, parent))) { mtp_err("delete of old version failed; temp kept"); invalidate(par, 0); return r; }
    if (LIBMTP_Set_Object_Filename(dev, tid, (char *)nm)) {
      LIBMTP_Clear_Errorstack(dev);
      /* rename unsupported: upload again under the real name, then drop temp */
      if ((r = send_fd(h->fd, parent, nm, want, &nid))) { invalidate(par, 0); return r; }
      if (find_named(parent, nm, &nid, &got) != 1 || got != want) { invalidate(par, 0); return -EIO; }
      safe_delete(tid, parent);
    } else nid = tid;
  }
  h->id = nid; h->size = want; h->dirty = 0;
  invalidate(par, 0);
  DBG("uploaded %s (%llu bytes, verified)\n", h->path, (unsigned long long)want);
  return 0;
}

/* -------------------------------------------------------------- FUSE ops */
static void fill_stat(const ent_t *e, struct stat *st) {
  memset(st, 0, sizeof *st);
  st->st_uid = getuid(); st->st_gid = getgid();
  if (e->isdir) { st->st_mode = S_IFDIR | (opt_ro ? 0555 : 0755); st->st_nlink = 2; }
  else { st->st_mode = S_IFREG | (opt_ro ? 0444 : 0644); st->st_nlink = 1; st->st_size = (off_t)e->size; st->st_blocks = (st->st_size + 511) / 512; }
  st->st_mtime = st->st_ctime = st->st_atime = e->mtime ? e->mtime : 0;
  st->st_blksize = 65536;
}
static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
  (void)fi;
  LOCK();
  int r = 0;
  if (is_junk(path)) {
    ph_t *p = ph_find(path);
    if (!p) r = -ENOENT;
    else { memset(st, 0, sizeof *st); st->st_mode = S_IFREG | 0644; st->st_nlink = 1; st->st_size = p->size; st->st_uid = getuid(); st->st_gid = getgid(); }
    UNLOCK(); return r;
  }
  fh_t *h = fh_pending(path);
  if (h) {
    struct stat ts; fstat(h->fd, &ts);
    ent_t e = { 0 }; e.size = (uint64_t)ts.st_size; e.mtime = time(NULL);
    fill_stat(&e, st); UNLOCK(); return 0;
  }
  ent_t e; r = lookup(path, &e);
  if (!r) fill_stat(&e, st);
  UNLOCK(); return r;
}
static int op_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
  (void)off; (void)fi; (void)fl;
  LOCK();
  dc_t *c = get_dir(path);
  if (!c) { UNLOCK(); return -ENOENT; }
  fill(buf, ".", NULL, 0, 0); fill(buf, "..", NULL, 0, 0);
  for (int i = 0; i < c->n; i++) {
    struct stat st; fill_stat(&c->e[i], &st);
    if (fill(buf, c->e[i].name, &st, 0, 0)) break;
  }
  /* new files still being written show up too */
  size_t L = strlen(path);
  for (fh_t *h = open_files; h; h = h->next) {
    if (h->junk || h->id || h->fd < 0) continue;
    char par[4096]; parent_of(h->path, par, sizeof par);
    if (strncmp(par, path, L) || par[L]) continue;
    fill(buf, base_of(h->path), NULL, 0, 0);
  }
  UNLOCK(); return 0;
}
static int op_open(const char *path, struct fuse_file_info *fi) {
  int acc = fi->flags & O_ACCMODE, wr = acc != O_RDONLY;
  if (wr && opt_ro) return -EROFS;
  LOCK();
  if (is_junk(path)) {
    ph_t *p = ph_find(path); if (!p) { UNLOCK(); return -ENOENT; }
    if (fi->flags & O_TRUNC) p->size = 0;
    fh_t *h = fh_new(path); h->junk = 1; fi->fh = (uint64_t)(uintptr_t)h; UNLOCK(); return 0;
  }
  ent_t e; int r = lookup(path, &e);
  if (r) { UNLOCK(); return r; }
  if (e.isdir) { UNLOCK(); return -EISDIR; }
  fh_t *h = fh_new(path); h->id = e.id; h->size = e.size; h->writable = wr;
  if (wr) {
    if (fi->flags & O_TRUNC) { r = fh_mktemp(h); h->dirty = 1; }
    else r = fh_fetch(h);
    if (r) { fh_drop(h); UNLOCK(); return r; }
  }
  fi->fh = (uint64_t)(uintptr_t)h;
  UNLOCK(); return 0;
}
static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
  (void)mode;
  if (opt_ro) return -EROFS;
  LOCK();
  if (is_junk(path)) {
    ph_t *p = ph_add(path); p->size = 0;
    fh_t *h = fh_new(path); h->junk = 1; fi->fh = (uint64_t)(uintptr_t)h; UNLOCK(); return 0;
  }
  char par[4096]; parent_of(path, par, sizeof par);
  if (!dir_id(par)) { UNLOCK(); return -ENOENT; }
  ent_t e;
  fh_t *h = fh_new(path); h->writable = 1; h->dirty = 1;
  if (!lookup(path, &e)) { if (e.isdir) { fh_drop(h); UNLOCK(); return -EISDIR; } h->id = e.id; h->size = e.size; }
  int r = fh_mktemp(h);
  if (r) { fh_drop(h); UNLOCK(); return r; }
  fi->fh = (uint64_t)(uintptr_t)h;
  UNLOCK(); return 0;
}
static int op_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
  (void)path;
  fh_t *h = (fh_t *)(uintptr_t)fi->fh;
  if (h->junk) return 0;
  LOCK();
  int r;
  if (h->fd < 0 && !no_partial && h->reads < 2 && h->size > 2 * 1024 * 1024) {
    /* small peeks (Finder previews) of big files: ranged read, no full download */
    h->reads++;
    if ((uint64_t)off >= h->size) { UNLOCK(); return 0; }
    unsigned char *data = NULL; unsigned int got = 0;
    uint32_t want = (uint32_t)(size > 1048576 ? 1048576 : size);
    if (LIBMTP_GetPartialObject(dev, h->id, (uint64_t)off, want, &data, &got) == 0) {
      if (got > size) got = (unsigned)size;
      memcpy(buf, data, got); free(data); UNLOCK(); return (int)got;
    }
    LIBMTP_Clear_Errorstack(dev); free(data);
    no_partial = 1; DBG("partial reads unsupported, using full downloads\n");
  }
  if ((r = fh_fetch(h))) { UNLOCK(); return r; }
  ssize_t n = pread(h->fd, buf, size, off);
  UNLOCK();
  return n < 0 ? -errno : (int)n;
}
static int op_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
  (void)path;
  fh_t *h = (fh_t *)(uintptr_t)fi->fh;
  if (opt_ro) return -EROFS;
  LOCK();
  if (h->junk) { ph_t *p = ph_find(h->path); if (p && off + (off_t)size > p->size) p->size = off + (off_t)size; UNLOCK(); return (int)size; }
  int r = fh_fetch(h); if (r) { UNLOCK(); return r; }
  ssize_t n = pwrite(h->fd, buf, size, off);
  if (n > 0) h->dirty = 1;
  UNLOCK();
  return n < 0 ? -errno : (int)n;
}
static int op_flush(const char *path, struct fuse_file_info *fi) {
  (void)path;
  fh_t *h = (fh_t *)(uintptr_t)fi->fh;
  LOCK(); int r = fh_commit(h); UNLOCK(); return r;
}
static int op_fsync(const char *path, int ds, struct fuse_file_info *fi) { (void)ds; return op_flush(path, fi); }
static int op_release(const char *path, struct fuse_file_info *fi) {
  (void)path;
  fh_t *h = (fh_t *)(uintptr_t)fi->fh;
  LOCK(); int r = fh_commit(h); if (r) fprintf(stderr, "upload of %s failed on close (%d)\n", h->path, r); fh_drop(h); UNLOCK(); return 0;
}
static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
  if (opt_ro) return -EROFS;
  LOCK();
  int r = 0;
  if (is_junk(path)) { ph_t *p = ph_find(path); if (p) p->size = size; UNLOCK(); return p ? 0 : -ENOENT; }
  fh_t *h = fi ? (fh_t *)(uintptr_t)fi->fh : fh_pending(path);
  if (h) {
    if ((r = fh_fetch(h))) { UNLOCK(); return r; }
    if (ftruncate(h->fd, size)) r = -errno; else h->dirty = 1;
    UNLOCK(); return r;
  }
  ent_t e; if ((r = lookup(path, &e))) { UNLOCK(); return r; }
  if (e.isdir) { UNLOCK(); return -EISDIR; }
  if ((uint64_t)size == e.size) { UNLOCK(); return 0; }
  fh_t *t = fh_new(path); t->id = e.id; t->size = e.size;
  if (size == 0) r = fh_mktemp(t); else r = fh_fetch(t);
  if (!r && ftruncate(t->fd, size)) r = -errno;
  if (!r) { t->dirty = 1; r = fh_commit(t); }
  fh_drop(t); UNLOCK(); return r;
}
static int op_mkdir(const char *path, mode_t mode) {
  (void)mode;
  if (opt_ro) return -EROFS;
  if (is_junk(path)) return -EPERM; /* no .Trashes / .fseventsd on the Kindle */
  LOCK();
  char par[4096]; parent_of(path, par, sizeof par);
  ent_t e;
  if (!lookup(path, &e)) { UNLOCK(); return -EEXIST; }
  uint32_t parent = dir_id(par); if (!parent) { UNLOCK(); return -ENOENT; }
  char *nm = strdup(base_of(path));
  uint32_t id = LIBMTP_Create_Folder(dev, nm, pid_for_create(parent), storage_id);
  free(nm);
  invalidate(par, 0);
  if (!id) { mtp_err("mkdir failed"); UNLOCK(); return -EIO; }
  DBG("mkdir %s (id %u)\n", path, id);
  UNLOCK(); return 0;
}
static int op_unlink(const char *path) {
  if (opt_ro) return -EROFS;
  LOCK();
  if (is_junk(path)) { int ok = ph_find(path) != NULL; ph_del(path); UNLOCK(); return ok ? 0 : -ENOENT; }
  ent_t e; int r = lookup(path, &e);
  if (r) { UNLOCK(); return r; }
  if (e.isdir) { UNLOCK(); return -EISDIR; }
  char par[4096]; parent_of(path, par, sizeof par);
  uint32_t parent = dir_id(par);
  r = safe_delete(e.id, parent);
  if (r) mtp_err("delete failed");
  invalidate(par, 0);
  DBG("deleted %s\n", path);
  UNLOCK(); return r;
}
static int op_rmdir(const char *path) {
  if (opt_ro) return -EROFS;
  if (!strcmp(path, "/")) return -EBUSY;
  LOCK();
  ent_t e; int r = lookup(path, &e);
  if (r) { UNLOCK(); return r; }
  if (!e.isdir) { UNLOCK(); return -ENOTDIR; }
  /* fresh listing: only empty folders can be removed */
  LIBMTP_file_t *ls = LIBMTP_Get_Files_And_Folders(dev, storage_id, e.id);
  int n = 0; while (ls) { LIBMTP_file_t *nx = ls->next; LIBMTP_destroy_file_t(ls); ls = nx; n++; }
  if (n) { invalidate(path, 1); UNLOCK(); return -ENOTEMPTY; }
  char par[4096]; parent_of(path, par, sizeof par);
  r = safe_delete(e.id, dir_id(par));
  if (r) mtp_err("rmdir failed");
  invalidate(path, 1); invalidate(par, 0);
  UNLOCK(); return r;
}
static int op_rename(const char *from, const char *to, unsigned int flags) {
  if (opt_ro) return -EROFS;
  if (flags & RENAME_EXCHANGE) return -EINVAL;
  LOCK();
  int jf = is_junk(from), jt = is_junk(to);
  if (jf || jt) {
    int r = 0;
    if (jf && jt) { ph_t *p = ph_find(from); if (!p) r = -ENOENT; else { off_t s = p->size; ph_del(from); ph_add(to)->size = s; } }
    else r = -EPERM;
    UNLOCK(); return r;
  }
  ent_t src, dst; int r = lookup(from, &src);
  if (r) { UNLOCK(); return r; }
  char fpar[4096], tpar[4096]; parent_of(from, fpar, sizeof fpar); parent_of(to, tpar, sizeof tpar);
  uint32_t fp = dir_id(fpar), tp = dir_id(tpar);
  if (!tp) { UNLOCK(); return -ENOENT; }
  if (!lookup(to, &dst)) {
    if (flags & RENAME_NOREPLACE) { UNLOCK(); return -EEXIST; }
    if (dst.id == src.id) { UNLOCK(); return 0; }
    if (dst.isdir != src.isdir) { UNLOCK(); return dst.isdir ? -EISDIR : -ENOTDIR; }
    if (dst.isdir) {
      LIBMTP_file_t *ls = LIBMTP_Get_Files_And_Folders(dev, storage_id, dst.id);
      int n = 0; while (ls) { LIBMTP_file_t *nx = ls->next; LIBMTP_destroy_file_t(ls); ls = nx; n++; }
      if (n) { UNLOCK(); return -ENOTEMPTY; }
    }
    if ((r = safe_delete(dst.id, tp))) { mtp_err("removing rename target failed"); UNLOCK(); return r; }
    invalidate(tpar, 0);
  }
  if (fp != tp) {
    if (LIBMTP_Move_Object(dev, src.id, storage_id, pid_for_create(tp))) {
      LIBMTP_Clear_Errorstack(dev); invalidate(fpar, 0); invalidate(tpar, 0); UNLOCK(); return -EXDEV; }
  }
  if (strcmp(base_of(from), base_of(to))) {
    if (LIBMTP_Set_Object_Filename(dev, src.id, (char *)base_of(to))) {
      mtp_err("rename failed"); invalidate(fpar, 0); invalidate(tpar, 0); UNLOCK(); return -EIO; }
  }
  invalidate(fpar, 0); invalidate(tpar, 0); if (src.isdir) { invalidate(from, 1); invalidate(to, 1); }
  for (fh_t *h = open_files; h; h = h->next) if (!strcmp(h->path, from)) snprintf(h->path, sizeof h->path, "%s", to);
  DBG("renamed %s -> %s\n", from, to);
  UNLOCK(); return 0;
}
static int op_statfs(const char *path, struct statvfs *s) {
  (void)path;
  LOCK();
  static time_t at = 0; static uint64_t freeb = 0, maxb = 0;
  if (time(NULL) - at > 10) {
    if (LIBMTP_Get_Storage(dev, LIBMTP_STORAGE_SORTBY_NOTSORTED) == 0 && dev->storage) {
      for (LIBMTP_devicestorage_t *st = dev->storage; st; st = st->next) if (st->id == storage_id) { freeb = st->FreeSpaceInBytes; maxb = st->MaxCapacity; }
    } else LIBMTP_Clear_Errorstack(dev);
    at = time(NULL);
  }
  UNLOCK();
  memset(s, 0, sizeof *s);
  s->f_bsize = s->f_frsize = 4096;
  s->f_blocks = maxb / 4096; s->f_bfree = s->f_bavail = freeb / 4096;
  s->f_files = 1000000; s->f_ffree = 1000000; s->f_namemax = 255;
  if (opt_ro) s->f_flag |= ST_RDONLY;
  return 0;
}
/* metadata changes are accepted and ignored (MTP can't store them) */
static int op_chmod(const char *p, mode_t m, struct fuse_file_info *fi) { (void)p; (void)m; (void)fi; return opt_ro ? -EROFS : 0; }
static int op_chown(const char *p, uid_t u, gid_t g, struct fuse_file_info *fi) { (void)p; (void)u; (void)g; (void)fi; return opt_ro ? -EROFS : 0; }
static int op_utimens(const char *p, const struct timespec tv[2], struct fuse_file_info *fi) { (void)p; (void)tv; (void)fi; return opt_ro ? -EROFS : 0; }
static int op_setxattr(const char *p, const char *n, const char *v, size_t s, int f) { (void)p; (void)n; (void)v; (void)s; (void)f; return opt_ro ? -EROFS : 0; }
static int op_getxattr(const char *p, const char *n, char *v, size_t s) { (void)p; (void)n; (void)v; (void)s; return -ENOATTR; }
static int op_listxattr(const char *p, char *l, size_t s) { (void)p; (void)l; (void)s; return 0; }
static int op_removexattr(const char *p, const char *n) { (void)p; (void)n; return opt_ro ? -EROFS : -ENOATTR; }

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
  (void)conn;
  mounted = 1;
  printf("mounted\n");
  cfg->entry_timeout = 2; cfg->attr_timeout = 2; cfg->negative_timeout = 1;
  cfg->hard_remove = 1;
  return NULL;
}

static const struct fuse_operations ops = {
  .init = op_init, .getattr = op_getattr, .readdir = op_readdir,
  .open = op_open, .create = op_create, .read = op_read, .write = op_write,
  .flush = op_flush, .fsync = op_fsync, .release = op_release, .truncate = op_truncate,
  .mkdir = op_mkdir, .unlink = op_unlink, .rmdir = op_rmdir, .rename = op_rename,
  .statfs = op_statfs, .chmod = op_chmod, .chown = op_chown, .utimens = op_utimens,
  .setxattr = op_setxattr, .getxattr = op_getxattr, .listxattr = op_listxattr, .removexattr = op_removexattr,
};

/* ------------------------------------------------------------------ main */
static void usage(FILE *o) {
  fprintf(o, "usage: kindle-mtpfs [--ro] [--debug] [--fuse-opt OPTS] /Volumes/Kindle\n"
             "  --ro        read-only: every change returns 'Read-only file system'\n"
             "  --debug     log each MTP operation\n"
             "  --fuse-opt  extra comma-separated macFUSE mount options\n"
             "Normally started by kindle-mount. Quit Calibre/kpush first.\n");
}
static void rmtree_tmp(void) {
  if (!*tmpdir) return;
  char cmd[1200]; snprintf(cmd, sizeof cmd, "/bin/rm -rf '%s'", tmpdir); /* our own temp dir on the Mac */
  (void)!system(cmd);
}
int main(int argc, char **argv) {
  km_filter_start();
  if (strcmp(LIBMTP_VERSION_STRING, "1.1.23")) { fprintf(stderr, "built for libmtp 1.1.23 only, found %s\n", LIBMTP_VERSION_STRING); return 6; }
  const char *mnt = NULL, *extra = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--ro")) opt_ro = 1;
    else if (!strcmp(argv[i], "--debug")) opt_debug = 1;
    else if (!strcmp(argv[i], "--fuse-opt") && i + 1 < argc) extra = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
    else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); usage(stderr); return 2; }
    else if (!mnt) mnt = argv[i];
    else { usage(stderr); return 2; }
  }
  if (!mnt) { usage(stderr); return 2; }

  LIBMTP_Init();
  LIBMTP_raw_device_t *raw = NULL; int nraw = 0;
  if (LIBMTP_Detect_Raw_Devices(&raw, &nraw) != LIBMTP_ERROR_NONE || nraw < 1) { fprintf(stderr, "no Kindle found (plugged in and unlocked?)\n"); return 3; }
  int k = -1;
  for (int i = 0; i < nraw; i++) if (raw[i].device_entry.vendor_id == KINDLE_VID && raw[i].device_entry.product_id == COLORSOFT1_PID) { k = i; break; }
  if (k < 0) { fprintf(stderr, "no Kindle Colorsoft (1949:9981) found\n"); return 3; }
  if (k) raw[0] = raw[k];
  raw[0].device_entry.device_flags &= ~KP_RESET_FLAG;
  dev = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
  if (!dev) { fprintf(stderr, "could not open the Kindle: it is probably busy. Quit Calibre, kpush or any other MTP app and try again. Not retrying.\n"); return 3; }
  if (no_reset_on_close(dev)) { LIBMTP_Release_Device(dev); return 5; }
  if (LIBMTP_Get_Storage(dev, LIBMTP_STORAGE_SORTBY_NOTSORTED) || !dev->storage) { fprintf(stderr, "no storage on device\n"); no_reset_on_close(dev); LIBMTP_Release_Device(dev); return 3; }
  storage_id = dev->storage->id;
  { char *m = LIBMTP_Get_Modelname(dev); printf("opened %s, storage %u, %s\n", m ? m : "Kindle", storage_id, opt_ro ? "read-only" : "read-write"); free(m); }

  const char *td = getenv("TMPDIR"); if (!td || !*td) td = "/tmp";
  snprintf(tmpdir, sizeof tmpdir, "%s/kindle-mtpfs.XXXXXX", td);
  if (!mkdtemp(tmpdir)) { perror("mkdtemp"); no_reset_on_close(dev); LIBMTP_Release_Device(dev); return 2; }

  char opts[1024];
  snprintf(opts, sizeof opts, "backend=fskit,volname=Kindle,fsname=Kindle%s%s%s",
           opt_ro ? ",ro" : "", extra ? "," : "", extra ? extra : "");
  char *fargv[] = { argv[0], "-f", "-s", "-o", opts, (char *)mnt, NULL };
  printf("mounting at %s (%s)\n", mnt, opts);
  int rc = fuse_main(6, fargv, &ops, NULL);

  /* unmounted: release the Kindle cleanly, still without a USB reset */
  for (fh_t *h = open_files; h;) { fh_t *n = h->next; fh_commit(h); fh_drop(h); h = n; }
  no_reset_on_close(dev);
  LIBMTP_Release_Device(dev);
  rmtree_tmp();
  if (!mounted) { fprintf(stderr, "mount failed; Kindle released\n"); return rc ? rc : 7; }
  printf("unmounted, Kindle released (rc %d)\n", rc);
  return rc;
}
