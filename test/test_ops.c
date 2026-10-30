/* unit tests: drive the FUSE ops directly against the in-memory mock device */
#define main kindle_mtpfs_main
#include "../src/kindle-mtpfs.c"
#undef main
#include <assert.h>
extern int deletes, partial_ok, rename_ok, move_ok;
uint32_t mock_add(uint32_t parent, const char *name, int isdir, const char *data);
int mock_count(void);
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static char listing[8192];
static int filler(void *b, const char *n, const struct stat *s, off_t o, enum fuse_fill_dir_flags f) { (void)b; (void)s; (void)o; (void)f; strcat(listing, n); strcat(listing, " "); return 0; }
static int ls(const char *p) { listing[0] = 0; return op_readdir(p, NULL, filler, 0, NULL, 0); }
static int writefile(const char *p, const char *data) {
  struct fuse_file_info fi = { O_WRONLY | O_CREAT | O_TRUNC, 0 };
  int r = op_create(p, 0644, &fi); if (r) return r;
  r = op_write(p, data, strlen(data), 0, &fi); if (r < 0) return r;
  struct stat st; CHECK(op_getattr(p, &st, NULL) == 0 && st.st_size == (off_t)strlen(data)); /* visible before close */
  r = op_flush(p, &fi); op_release(p, &fi); return r;
}
static int readfile(const char *p, char *out, size_t n) {
  struct fuse_file_info fi = { O_RDONLY, 0 };
  int r = op_open(p, &fi); if (r) return r;
  r = op_read(p, out, n, 0, &fi); op_release(p, &fi); if (r >= 0) out[r] = 0; return r;
}
static void reset_cache(void) { while (dcache) { dc_t *n = dcache->next; dc_free(dcache); dcache = n; } }
int main(void) {
  static LIBMTP_mtpdevice_t d; dev = &d; storage_id = 1;
  snprintf(tmpdir, sizeof tmpdir, "/tmp/kmtest.XXXXXX"); mkdtemp(tmpdir);
  uint32_t docs = mock_add(ROOT, "documents", 1, NULL);
  uint32_t kor = mock_add(ROOT, "koreader", 1, NULL);
  mock_add(docs, "book.epub", 0, "EPUBDATA-1234567890");
  mock_add(kor, "_meta.lua", 0, "return { name = 'x' }\n");
  struct stat st; char buf[4096];
  /* read side */
  CHECK(ls("/") == 0 && strstr(listing, "documents") && strstr(listing, "koreader"));
  CHECK(op_getattr("/documents/book.epub", &st, NULL) == 0 && st.st_size == 19 && S_ISREG(st.st_mode));
  CHECK(readfile("/koreader/_meta.lua", buf, sizeof buf) == 22 && !strcmp(buf, "return { name = 'x' }\n"));
  CHECK(op_getattr("/nope", &st, NULL) == -ENOENT);
  int d0 = deletes;
  /* scratch-folder flow, same as the device test */
  CHECK(op_mkdir("/documents/_mount_test", 0755) == 0);
  CHECK(op_mkdir("/documents/_mount_test", 0755) == -EEXIST);
  CHECK(writefile("/documents/_mount_test/hello.txt", "hello kindle\n") == 0);
  CHECK(readfile("/documents/_mount_test/hello.txt", buf, sizeof buf) == 13 && !strcmp(buf, "hello kindle\n"));
  /* overwrite existing: temp upload + replace, exactly one copy left */
  CHECK(writefile("/documents/_mount_test/hello.txt", "second version!\n") == 0);
  reset_cache();
  CHECK(readfile("/documents/_mount_test/hello.txt", buf, sizeof buf) == 16 && !strcmp(buf, "second version!\n"));
  CHECK(ls("/documents/_mount_test") == 0 && !strstr(listing, "kmtmp"));
  CHECK(deletes == d0 + 1);
  /* overwrite when device rename is unsupported: falls back, no temp left */
  rename_ok = 0;
  CHECK(writefile("/documents/_mount_test/hello.txt", "third\n") == 0);
  rename_ok = 1; reset_cache();
  CHECK(readfile("/documents/_mount_test/hello.txt", buf, sizeof buf) == 6);
  CHECK(ls("/documents/_mount_test") == 0 && !strstr(listing, "kmtmp"));
  /* append via open without O_TRUNC */
  { struct fuse_file_info fi = { O_WRONLY, 0 }; CHECK(op_open("/documents/_mount_test/hello.txt", &fi) == 0);
    CHECK(op_write("x", "more\n", 5, 6, &fi) == 5); CHECK(op_flush("x", &fi) == 0); op_release("x", &fi); }
  reset_cache(); CHECK(readfile("/documents/_mount_test/hello.txt", buf, sizeof buf) == 11 && !strcmp(buf, "third\nmore\n"));
  /* truncate by path */
  CHECK(op_truncate("/documents/_mount_test/hello.txt", 3, NULL) == 0);
  reset_cache(); CHECK(readfile("/documents/_mount_test/hello.txt", buf, sizeof buf) == 3);
  /* rename in place */
  CHECK(op_rename("/documents/_mount_test/hello.txt", "/documents/_mount_test/renamed.txt", 0) == 0);
  CHECK(op_getattr("/documents/_mount_test/hello.txt", &st, NULL) == -ENOENT);
  CHECK(op_getattr("/documents/_mount_test/renamed.txt", &st, NULL) == 0 && st.st_size == 3);
  /* mkdir sub, move file into it, move back */
  CHECK(op_mkdir("/documents/_mount_test/sub", 0755) == 0);
  CHECK(op_rename("/documents/_mount_test/renamed.txt", "/documents/_mount_test/sub/moved.txt", 0) == 0);
  CHECK(op_getattr("/documents/_mount_test/sub/moved.txt", &st, NULL) == 0);
  move_ok = 0;
  CHECK(op_rename("/documents/_mount_test/sub/moved.txt", "/documents/_mount_test/back.txt", 0) == -EXDEV);
  move_ok = 1;
  CHECK(op_getattr("/documents/_mount_test/sub/moved.txt", &st, NULL) == 0);
  /* rmdir non-empty refused */
  CHECK(op_rmdir("/documents/_mount_test/sub") == -ENOTEMPTY);
  CHECK(op_rmdir("/documents/_mount_test/sub/moved.txt") == -ENOTDIR);
  CHECK(op_unlink("/documents/_mount_test/sub") == -EISDIR);
  /* macOS clutter never reaches the device */
  int before = mock_count();
  CHECK(writefile("/documents/_mount_test/.DS_Store", "junk") == 0);
  CHECK(writefile("/documents/_mount_test/._renamed.txt", "junk") == 0);
  CHECK(op_mkdir("/.Trashes", 0755) == -EPERM);
  int after = mock_count();
  CHECK(before == after);
  CHECK(op_getattr("/documents/_mount_test/.DS_Store", &st, NULL) == 0);
  CHECK(ls("/documents/_mount_test") == 0 && !strstr(listing, ".DS_Store"));
  CHECK(op_unlink("/documents/_mount_test/.DS_Store") == 0 && op_unlink("/documents/_mount_test/._renamed.txt") == 0);
  /* clean up scratch */
  CHECK(op_unlink("/documents/_mount_test/sub/moved.txt") == 0);
  CHECK(op_rmdir("/documents/_mount_test/sub") == 0);
  CHECK(op_rmdir("/documents/_mount_test") == 0);
  CHECK(op_getattr("/documents/_mount_test", &st, NULL) == -ENOENT);
  CHECK(op_getattr("/documents/book.epub", &st, NULL) == 0);
  CHECK(op_rmdir("/") == -EBUSY);
  /* partial reads for big files */
  { static char big[3 * 1024 * 1024 + 1]; memset(big, 'a', sizeof big - 1); big[100] = 'Z';
    mock_add(docs, "big.pdf", 0, big); reset_cache();
    struct fuse_file_info fi = { O_RDONLY, 0 }; CHECK(op_open("/documents/big.pdf", &fi) == 0);
    fh_t *h = (fh_t *)(uintptr_t)fi.fh;
    CHECK(op_read("x", buf, 200, 0, &fi) == 200 && buf[100] == 'Z' && h->fd < 0);
    CHECK(op_read("x", buf, 10, 3 * 1024 * 1024 - 5, &fi) == 5);
    CHECK(op_read("x", buf, 10, 0, &fi) == 10 && h->fd >= 0); /* third read: full download */
    op_release("x", &fi); }
  /* read-only mode */
  opt_ro = 1;
  CHECK(op_mkdir("/documents/x", 0755) == -EROFS);
  CHECK(op_unlink("/documents/book.epub") == -EROFS);
  CHECK(op_rename("/documents/book.epub", "/documents/b.epub", 0) == -EROFS);
  { struct fuse_file_info fi = { O_WRONLY, 0 }; CHECK(op_open("/documents/book.epub", &fi) == -EROFS);
    CHECK(op_create("/documents/n.txt", 0644, &fi) == -EROFS); }
  opt_ro = 0;
  rmtree_tmp();
  printf("%s (%d failures), deletes %d\n", fails ? "TESTS FAILED" : "all tests passed", fails, deletes);
  return fails != 0;
}
