/* in-memory MTP device for tests; counts deletes so tests can assert on them */
#include "stub/libmtp.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
typedef struct { int used; uint32_t parent; int isdir; char name[256]; char *data; size_t len; } obj_t;
obj_t objs[4096]; int deletes = 0, partial_ok = 1, rename_ok = 1, move_ok = 1;
static uint32_t nextid = 1;
uint32_t mock_add(uint32_t parent, const char *name, int isdir, const char *data) {
  uint32_t id = nextid++; obj_t *o = &objs[id]; o->used = 1; o->parent = parent; o->isdir = isdir; snprintf(o->name, 256, "%s", name);
  if (data) { o->len = strlen(data); o->data = strdup(data); } return id;
}
static uint32_t norm(uint32_t p) { return p == 0 ? LIBMTP_FILES_AND_FOLDERS_ROOT : p; }
LIBMTP_file_t *LIBMTP_Get_Files_And_Folders(LIBMTP_mtpdevice_t *d, uint32_t s, uint32_t parent) {
  (void)d; (void)s; LIBMTP_file_t *head = NULL;
  for (uint32_t i = 1; i < nextid; i++) if (objs[i].used && objs[i].parent == parent) {
    LIBMTP_file_t *f = LIBMTP_new_file_t(); f->item_id = i; f->parent_id = parent; f->filename = strdup(objs[i].name);
    f->filesize = objs[i].len; f->filetype = objs[i].isdir ? LIBMTP_FILETYPE_FOLDER : LIBMTP_FILETYPE_UNKNOWN; f->modificationdate = 1000; f->next = head; head = f; }
  return head;
}
void LIBMTP_destroy_file_t(LIBMTP_file_t *f) { if (f) { free(f->filename); free(f); } }
LIBMTP_file_t *LIBMTP_new_file_t(void) { return calloc(1, sizeof(LIBMTP_file_t)); }
void LIBMTP_Clear_Errorstack(LIBMTP_mtpdevice_t *d) { (void)d; }
void LIBMTP_Dump_Errorstack(LIBMTP_mtpdevice_t *d) { (void)d; }
int LIBMTP_Get_File_To_File_Descriptor(LIBMTP_mtpdevice_t *d, uint32_t id, int fd, LIBMTP_progressfunc_t p, void const *x) {
  (void)d; (void)p; (void)x; if (!objs[id].used || objs[id].isdir) return -1; return write(fd, objs[id].data, objs[id].len) == (ssize_t)objs[id].len ? 0 : -1; }
int LIBMTP_Send_File_From_File_Descriptor(LIBMTP_mtpdevice_t *d, int fd, LIBMTP_file_t *f, LIBMTP_progressfunc_t p, void const *x) {
  (void)d; (void)p; (void)x; char *buf = malloc(f->filesize + 1); ssize_t n = read(fd, buf, f->filesize);
  if (n != (ssize_t)f->filesize) { free(buf); return -1; }
  uint32_t id = mock_add(norm(f->parent_id), f->filename, 0, NULL); objs[id].data = buf; objs[id].len = n; f->item_id = id; return 0; }
int LIBMTP_Delete_Object(LIBMTP_mtpdevice_t *d, uint32_t id) { (void)d; if (!objs[id].used) return -1; objs[id].used = 0; deletes++; return 0; }
int LIBMTP_Set_Object_Filename(LIBMTP_mtpdevice_t *d, uint32_t id, char *n) { (void)d; if (!rename_ok || !objs[id].used) return -1; snprintf(objs[id].name, 256, "%s", n); return 0; }
uint32_t LIBMTP_Create_Folder(LIBMTP_mtpdevice_t *d, char *n, uint32_t p, uint32_t s) { (void)d; (void)s; return mock_add(norm(p), n, 1, NULL); }
int LIBMTP_Move_Object(LIBMTP_mtpdevice_t *d, uint32_t id, uint32_t s, uint32_t p) { (void)d; (void)s; if (!move_ok) return -1; objs[id].parent = norm(p); return 0; }
int LIBMTP_GetPartialObject(LIBMTP_mtpdevice_t *d, uint32_t id, uint64_t off, uint32_t max, unsigned char **data, unsigned int *got) {
  (void)d; if (!partial_ok) return -1; size_t L = objs[id].len; size_t n = off >= L ? 0 : (L - off < max ? L - off : max);
  *data = malloc(n + 1); memcpy(*data, objs[id].data + off, n); *got = n; return 0; }
int LIBMTP_Get_Storage(LIBMTP_mtpdevice_t *d, int s) { (void)d; (void)s; return 0; }
void LIBMTP_Init(void) {}
LIBMTP_error_number_t LIBMTP_Detect_Raw_Devices(LIBMTP_raw_device_t **r, int *n) { (void)r; *n = 0; return LIBMTP_ERROR_GENERAL; }
LIBMTP_mtpdevice_t *LIBMTP_Open_Raw_Device_Uncached(LIBMTP_raw_device_t *r) { (void)r; return NULL; }
void LIBMTP_Release_Device(LIBMTP_mtpdevice_t *d) { (void)d; }
char *LIBMTP_Get_Modelname(LIBMTP_mtpdevice_t *d) { (void)d; return NULL; }
int mock_count(void) { int n = 0; for (int i = 1; i < 4096; i++) n += objs[i].used; return n; }
