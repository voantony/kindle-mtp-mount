/* redact.h - serial-redacting output filter (MIT, see LICENSE).
 * Forks a small child that reads our stdout/stderr through pipes and writes
 * them to the real stdout/stderr with anything that looks like a Kindle serial
 * (G followed by 3+ characters of [0-9A-Z]) replaced by "GR…(redacted)".
 * Covers libmtp's own messages too, since they go through the same fds. */
#ifndef KM_REDACT_H
#define KM_REDACT_H
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

static pid_t km_filt_pid = -1;
static void km_wr(int fd, const char *s, size_t n) {
  while (n) { ssize_t w = write(fd, s, n); if (w <= 0) { if (errno == EINTR) continue; return; } s += w; n -= (size_t)w; }
}
static int km_serialch(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }
static void km_redact_out(int fd, const char *s, size_t n) {
  static const char rep[] = "GR\xe2\x80\xa6(redacted)";
  size_t i = 0, st = 0;
  while (i < n) {
    if (s[i] == 'G') {
      size_t j = i + 1; while (j < n && km_serialch(s[j])) j++;
      if (j - i - 1 >= 3) { km_wr(fd, s + st, i - st); km_wr(fd, rep, sizeof rep - 1); i = j; st = j; continue; }
    }
    i++;
  }
  km_wr(fd, s + st, n - st);
}
typedef struct { int in, out; char buf[65536]; size_t len; int open; } km_stream;
static void km_flush_lines(km_stream *f, int all) {
  size_t start = 0;
  for (size_t i = 0; i < f->len; i++) if (f->buf[i] == '\n') { km_redact_out(f->out, f->buf + start, i + 1 - start); start = i + 1; }
  if (all || (start == 0 && f->len == sizeof f->buf)) { km_redact_out(f->out, f->buf + start, f->len - start); start = f->len; }
  memmove(f->buf, f->buf + start, f->len - start); f->len -= start;
}
static void km_filter_child(int r1, int o1, int r2, int o2) {
  signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
  static km_stream s[2];
  s[0].in = r1; s[0].out = o1; s[0].open = 1; s[1].in = r2; s[1].out = o2; s[1].open = 1;
  while (s[0].open || s[1].open) {
    struct pollfd p[2]; int np = 0, map[2];
    for (int k = 0; k < 2; k++) if (s[k].open) { p[np].fd = s[k].in; p[np].events = POLLIN; map[np] = k; np++; }
    if (poll(p, np, -1) < 0) { if (errno == EINTR) continue; break; }
    for (int q = 0; q < np; q++) if (p[q].revents) {
      km_stream *f = &s[map[q]];
      ssize_t r = read(f->in, f->buf + f->len, sizeof f->buf - f->len);
      if (r <= 0) { if (r < 0 && errno == EINTR) continue; km_flush_lines(f, 1); f->open = 0; continue; }
      f->len += (size_t)r; km_flush_lines(f, 0);
    }
  }
  _exit(0);
}
static void km_filter_finish(void) {
  if (km_filt_pid <= 0) return;
  fflush(stdout); fflush(stderr);
  int dn = open("/dev/null", O_WRONLY); dup2(dn, 1); dup2(dn, 2); close(dn);
  /* wait up to 2 s: a helper process that inherited the pipe must not hang us */
  for (int i = 0; i < 200; i++) { int st; if (waitpid(km_filt_pid, &st, WNOHANG) != 0) break; usleep(10000); }
  km_filt_pid = -1;
}
static void km_filter_start(void) {
  int a[2], b[2]; if (pipe(a) || pipe(b)) return;
  int o1 = dup(1), o2 = dup(2);
  pid_t p = fork(); if (p < 0) return;
  if (p == 0) { close(a[1]); close(b[1]); km_filter_child(a[0], o1, b[0], o2); }
  close(a[0]); close(b[0]); close(o1); close(o2);
  dup2(a[1], 1); dup2(b[1], 2); close(a[1]); close(b[1]);
  setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
  km_filt_pid = p; atexit(km_filter_finish);
}
#endif
