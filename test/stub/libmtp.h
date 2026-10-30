/* minimal in-memory stand-in for libmtp, for unit tests only */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#define LIBMTP_VERSION_STRING "1.1.23"
#define LIBMTP_FILES_AND_FOLDERS_ROOT 0xffffffff
typedef enum { LIBMTP_FILETYPE_FOLDER = 0, LIBMTP_FILETYPE_UNKNOWN = 44 } LIBMTP_filetype_t;
typedef enum { LIBMTP_ERROR_NONE = 0, LIBMTP_ERROR_GENERAL = 1 } LIBMTP_error_number_t;
#define LIBMTP_STORAGE_SORTBY_NOTSORTED 0
typedef int (*LIBMTP_progressfunc_t)(uint64_t, uint64_t, void const *);
typedef struct LIBMTP_file_struct { uint32_t item_id, parent_id, storage_id; char *filename; uint64_t filesize; time_t modificationdate; LIBMTP_filetype_t filetype; struct LIBMTP_file_struct *next; } LIBMTP_file_t;
typedef struct LIBMTP_devicestorage_struct { uint32_t id; uint64_t MaxCapacity, FreeSpaceInBytes; char *StorageDescription; struct LIBMTP_devicestorage_struct *next; } LIBMTP_devicestorage_t;
typedef struct { char *vendor; uint16_t vendor_id; char *product; uint16_t product_id; uint32_t device_flags; } LIBMTP_device_entry_t;
typedef struct { LIBMTP_device_entry_t device_entry; uint32_t bus_location; uint8_t devnum; } LIBMTP_raw_device_t;
typedef struct { void *usbinfo; LIBMTP_devicestorage_t *storage; } LIBMTP_mtpdevice_t;
LIBMTP_file_t *LIBMTP_Get_Files_And_Folders(LIBMTP_mtpdevice_t *, uint32_t, uint32_t);
void LIBMTP_destroy_file_t(LIBMTP_file_t *);
LIBMTP_file_t *LIBMTP_new_file_t(void);
void LIBMTP_Clear_Errorstack(LIBMTP_mtpdevice_t *);
void LIBMTP_Dump_Errorstack(LIBMTP_mtpdevice_t *);
int LIBMTP_Get_File_To_File_Descriptor(LIBMTP_mtpdevice_t *, uint32_t, int, LIBMTP_progressfunc_t, void const *);
int LIBMTP_Send_File_From_File_Descriptor(LIBMTP_mtpdevice_t *, int, LIBMTP_file_t *, LIBMTP_progressfunc_t, void const *);
int LIBMTP_Delete_Object(LIBMTP_mtpdevice_t *, uint32_t);
int LIBMTP_Set_Object_Filename(LIBMTP_mtpdevice_t *, uint32_t, char *);
uint32_t LIBMTP_Create_Folder(LIBMTP_mtpdevice_t *, char *, uint32_t, uint32_t);
int LIBMTP_Move_Object(LIBMTP_mtpdevice_t *, uint32_t, uint32_t, uint32_t);
int LIBMTP_GetPartialObject(LIBMTP_mtpdevice_t *, uint32_t, uint64_t, uint32_t, unsigned char **, unsigned int *);
int LIBMTP_Get_Storage(LIBMTP_mtpdevice_t *, int);
void LIBMTP_Init(void);
LIBMTP_error_number_t LIBMTP_Detect_Raw_Devices(LIBMTP_raw_device_t **, int *);
LIBMTP_mtpdevice_t *LIBMTP_Open_Raw_Device_Uncached(LIBMTP_raw_device_t *);
void LIBMTP_Release_Device(LIBMTP_mtpdevice_t *);
char *LIBMTP_Get_Modelname(LIBMTP_mtpdevice_t *);
