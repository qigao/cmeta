/**
 * @file cmeta_fs_root.c
 * @brief Secure root-relative filesystem capabilities.
 *
 * The capability path never concatenates an admitted host root with a guest
 * path. POSIX walks directory descriptors with openat(..., O_NOFOLLOW).
 * Windows walks native directory HANDLEs with NtCreateFile RootDirectory and
 * FILE_OPEN_REPARSE_POINT. Intermediate links/reparse points are rejected.
 */
#include "cmeta_fs.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ELOOP
#define ELOOP EINVAL
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG ERANGE
#endif
#ifndef EOVERFLOW
#define EOVERFLOW ERANGE
#endif

typedef struct cmeta_fs_root_path_cursor_s {
  const char *path;
  size_t offset;
  unsigned depth;
} cmeta_fs_root_path_cursor_t;

static bool cmeta_fs_root_is_separator(char ch) {
  return ch == '/' || ch == '\\';
}

static bool cmeta_fs_root_host_path_is_absolute(const char *path) {
  size_t length;
  if (!path || path[0] == '\0')
    return false;
  length = strlen(path);
#ifdef _WIN32
  if (length >= 3u && path[1] == ':' &&
      cmeta_fs_root_is_separator(path[2]))
    return true;
  return length >= 2u &&
      cmeta_fs_root_is_separator(path[0]) &&
      cmeta_fs_root_is_separator(path[1]);
#else
  return path[0] == '/';
#endif
}

static int cmeta_fs_root_next_component(
    cmeta_fs_root_path_cursor_t *cursor,
    char component[SALTS_FS_ROOT_COMPONENT_MAX],
    bool *out_last) {
  size_t start;
  size_t end;
  size_t length;

  if (!cursor || !cursor->path || !component || !out_last)
    return -EINVAL;
  if (cursor->offset == 0u) {
    if (cursor->path[0] == '\0' ||
        cmeta_fs_root_is_separator(cursor->path[0]))
      return -EINVAL;
  }
  if (++cursor->depth > SALTS_FS_ROOT_DEPTH_MAX)
    return -ENAMETOOLONG;

  start = cursor->offset;
  end = start;
  while (cursor->path[end] != '\0' &&
         !cmeta_fs_root_is_separator(cursor->path[end]))
    ++end;
  length = end - start;
  if (length == 0u || length >= SALTS_FS_ROOT_COMPONENT_MAX)
    return length == 0u ? -EINVAL : -ENAMETOOLONG;
  if ((length == 1u && cursor->path[start] == '.') ||
      (length == 2u && cursor->path[start] == '.' &&
       cursor->path[start + 1u] == '.'))
    return -EINVAL;
#ifdef _WIN32
  for (size_t i = start; i < end; ++i)
    if (cursor->path[i] == ':')
      return -EINVAL;
#endif
  memcpy(component, cursor->path + start, length);
  component[length] = '\0';

  if (cursor->path[end] == '\0') {
    cursor->offset = end;
    *out_last = true;
    return 0;
  }
  if (cursor->path[end + 1u] == '\0' ||
      cmeta_fs_root_is_separator(cursor->path[end + 1u]))
    return -EINVAL;
  cursor->offset = end + 1u;
  *out_last = false;
  return 0;
}

static int cmeta_fs_root_validate_relative_path(const char *path) {
  cmeta_fs_root_path_cursor_t cursor = {path, 0u, 0u};
  for (;;) {
    char component[SALTS_FS_ROOT_COMPONENT_MAX];
    bool last = false;
    int rc = cmeta_fs_root_next_component(&cursor, component, &last);
    if (rc != 0)
      return rc;
    if (last)
      return 0;
  }
}

static int cmeta_fs_root_validate_open_flags(int flags) {
  const int access =
      flags & (SALTS_FS_O_RDONLY | SALTS_FS_O_WRONLY | SALTS_FS_O_RDWR);
  const int known =
      SALTS_FS_O_RDONLY | SALTS_FS_O_WRONLY | SALTS_FS_O_RDWR |
      SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC | SALTS_FS_O_APPEND | SALTS_FS_ROOT_O_EXCL;
  if ((flags & ~known) != 0)
    return -EINVAL;
  if (access != SALTS_FS_O_RDONLY &&
      access != SALTS_FS_O_WRONLY &&
      access != SALTS_FS_O_RDWR)
    return -EINVAL;
  if ((flags & SALTS_FS_O_TRUNC) != 0 &&
      access == SALTS_FS_O_RDONLY)
    return -EINVAL;
  return 0;
}

#ifdef _WIN32

#include <sys/stat.h>
#include <winternl.h>

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001UL
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040UL
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000UL
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001UL
#endif
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002UL
#endif
#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 0x00000003UL
#endif
#ifndef FILE_OVERWRITE
#define FILE_OVERWRITE 0x00000004UL
#endif
#ifndef FILE_OVERWRITE_IF
#define FILE_OVERWRITE_IF 0x00000005UL
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040L
#endif

typedef NTSTATUS (NTAPI *cmeta_nt_create_file_fn)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef ULONG (WINAPI *cmeta_rtl_status_to_dos_fn)(NTSTATUS);
typedef NTSTATUS (NTAPI *cmeta_nt_set_information_file_fn)(
    HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
/* winternl.h's user-mode enum omits the documented FileRenameInformation. */
enum { CMETA_FS_FILE_RENAME_INFORMATION = 10 };

static INIT_ONCE cmeta_fs_root_nt_once = INIT_ONCE_STATIC_INIT;
static cmeta_nt_create_file_fn cmeta_fs_root_nt_create_file = NULL;
static cmeta_rtl_status_to_dos_fn cmeta_fs_root_status_to_dos = NULL;
static cmeta_nt_set_information_file_fn cmeta_fs_root_nt_set_information = NULL;

static BOOL CALLBACK cmeta_fs_root_init_nt(
    PINIT_ONCE once, PVOID parameter, PVOID *context) {
  HMODULE module;
  (void)once;
  (void)parameter;
  (void)context;
  module = GetModuleHandleW(L"ntdll.dll");
  if (!module)
    return FALSE;
  cmeta_fs_root_nt_create_file =
      (cmeta_nt_create_file_fn)GetProcAddress(module, "NtCreateFile");
  cmeta_fs_root_status_to_dos =
      (cmeta_rtl_status_to_dos_fn)GetProcAddress(
          module, "RtlNtStatusToDosError");
  cmeta_fs_root_nt_set_information =
      (cmeta_nt_set_information_file_fn)GetProcAddress(module, "NtSetInformationFile");
  return cmeta_fs_root_nt_create_file != NULL &&
         cmeta_fs_root_status_to_dos != NULL &&
         cmeta_fs_root_nt_set_information != NULL;
}

static int cmeta_fs_root_win32_error(DWORD error) {
  switch (error) {
  case ERROR_SUCCESS: return 0;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
    return -ENOENT;
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
  case ERROR_SHARING_VIOLATION:
    return -EACCES;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return -EEXIST;
  case ERROR_NOT_SAME_DEVICE:
    return -EXDEV;
  case ERROR_DIRECTORY:
    return -ENOTDIR;
  case ERROR_DIR_NOT_EMPTY:
    return -ENOTEMPTY;
  case ERROR_FILENAME_EXCED_RANGE:
    return -ENAMETOOLONG;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    return -ENOMEM;
  case ERROR_DISK_FULL:
    return -ENOSPC;
  case ERROR_INVALID_PARAMETER:
  case ERROR_INVALID_NAME:
    return -EINVAL;
  default:
    return -EIO;
  }
}

static int cmeta_fs_root_nt_error(NTSTATUS status) {
  if (!InitOnceExecuteOnce(
          &cmeta_fs_root_nt_once, cmeta_fs_root_init_nt, NULL, NULL))
    return -EIO;
  return cmeta_fs_root_win32_error(
      (DWORD)cmeta_fs_root_status_to_dos(status));
}

struct cmeta_fs_root_s {
  HANDLE handle;
};

struct cmeta_fs_root_file_s {
  HANDLE handle;
  bool append;
};

#define SALTS_FS_ROOT_WIN_DIR_BUFFER 16384u

struct cmeta_fs_root_dir_s {
  HANDLE handle;
  uint64_t cookie;
  unsigned char buffer[SALTS_FS_ROOT_WIN_DIR_BUFFER];
  ULONG offset;
  bool have_buffer;
  bool restart;
};

static int cmeta_fs_root_utf8_component_to_wide(
    const char *component,
    WCHAR wide[SALTS_FS_ROOT_COMPONENT_MAX],
    USHORT *out_length_bytes) {
  int count;
  if (!component || !wide || !out_length_bytes)
    return -EINVAL;
  count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, component, -1,
      wide, (int)SALTS_FS_ROOT_COMPONENT_MAX);
  if (count <= 0)
    return cmeta_fs_root_win32_error(GetLastError());
  if ((size_t)count * sizeof(WCHAR) > USHRT_MAX)
    return -ENAMETOOLONG;
  *out_length_bytes = (USHORT)((count - 1) * sizeof(WCHAR));
  return 0;
}

static int cmeta_fs_root_open_component_win(
    HANDLE parent,
    const char *component,
    ACCESS_MASK desired_access,
    ULONG disposition,
    ULONG create_options,
    ULONG file_attributes,
    bool allow_reparse,
    HANDLE *out_handle) {
  WCHAR wide[SALTS_FS_ROOT_COMPONENT_MAX];
  USHORT length_bytes;
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK io;
  HANDLE handle = INVALID_HANDLE_VALUE;
  FILE_ATTRIBUTE_TAG_INFO tag;
  NTSTATUS status;
  int rc;

  if (!out_handle || parent == NULL || parent == INVALID_HANDLE_VALUE)
    return -EINVAL;
  *out_handle = INVALID_HANDLE_VALUE;
  rc = cmeta_fs_root_utf8_component_to_wide(
      component, wide, &length_bytes);
  if (rc != 0)
    return rc;
  if (!InitOnceExecuteOnce(
          &cmeta_fs_root_nt_once, cmeta_fs_root_init_nt, NULL, NULL))
    return -EIO;

  name.Length = length_bytes;
  name.MaximumLength = (USHORT)(length_bytes + sizeof(WCHAR));
  name.Buffer = wide;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  memset(&io, 0, sizeof(io));

  status = cmeta_fs_root_nt_create_file(
      &handle,
      desired_access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      &attributes,
      &io,
      NULL,
      file_attributes,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      disposition,
      create_options | FILE_OPEN_REPARSE_POINT |
          FILE_SYNCHRONOUS_IO_NONALERT,
      NULL,
      0u);
  if (status < 0)
    return cmeta_fs_root_nt_error(status);

  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
    rc = cmeta_fs_root_win32_error(GetLastError());
    CloseHandle(handle);
    return rc;
  }
  if (!allow_reparse &&
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
    CloseHandle(handle);
    return -ELOOP;
  }

  *out_handle = handle;
  return 0;
}

static int cmeta_fs_root_open_parent_win(
    const cmeta_fs_root_t *root,
    const char *path,
    HANDLE *out_parent,
    bool *out_parent_owned,
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX]) {
  cmeta_fs_root_path_cursor_t cursor = {path, 0u, 0u};
  HANDLE current;
  bool current_owned = false;

  if (!root || !path || !out_parent || !out_parent_owned ||
      !final_component || root->handle == INVALID_HANDLE_VALUE)
    return -EINVAL;
  {
    int rc = cmeta_fs_root_validate_relative_path(path);
    if (rc != 0)
      return rc;
  }
  current = root->handle;
  for (;;) {
    char component[SALTS_FS_ROOT_COMPONENT_MAX];
    bool last = false;
    int rc = cmeta_fs_root_next_component(&cursor, component, &last);
    if (rc != 0) {
      if (current_owned) CloseHandle(current);
      return rc;
    }
    if (last) {
      memcpy(final_component, component, strlen(component) + 1u);
      *out_parent = current;
      *out_parent_owned = current_owned;
      return 0;
    }

    HANDLE next = INVALID_HANDLE_VALUE;
    rc = cmeta_fs_root_open_component_win(
        current, component,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE,
        FILE_OPEN, FILE_DIRECTORY_FILE,
        FILE_ATTRIBUTE_NORMAL, false, &next);
    if (current_owned) CloseHandle(current);
    if (rc != 0)
      return rc;
    current = next;
    current_owned = true;
  }
}

static uint64_t cmeta_fs_root_filetime_us(LARGE_INTEGER time) {
  const uint64_t epoch = 116444736000000000ULL;
  uint64_t ticks = (uint64_t)time.QuadPart;
  return ticks < epoch ? 0u : (ticks - epoch) / 10u;
}

static int cmeta_fs_root_stat_handle_win(
    HANDLE handle, cmeta_fs_stat_t *out_stat) {
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  if (!out_stat || handle == NULL || handle == INVALID_HANDLE_VALUE)
    return -EINVAL;
  if (!GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)))
    return cmeta_fs_root_win32_error(GetLastError());
  if (!GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)))
    return cmeta_fs_root_win32_error(GetLastError());

  memset(out_stat, 0, sizeof(*out_stat));
  out_stat->size = (uint64_t)standard.EndOfFile.QuadPart;
  out_stat->atime = cmeta_fs_root_filetime_us(basic.LastAccessTime);
  out_stat->mtime = cmeta_fs_root_filetime_us(basic.LastWriteTime);
  out_stat->ctime = cmeta_fs_root_filetime_us(basic.ChangeTime);
  out_stat->is_symlink =
      (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
  out_stat->is_directory =
      (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
  out_stat->is_file = !out_stat->is_directory && !out_stat->is_symlink;
  out_stat->mode = out_stat->is_directory ? _S_IFDIR : _S_IFREG;
  out_stat->mode |= _S_IREAD;
  if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0u)
    out_stat->mode |= _S_IWRITE;
  return 0;
}

static ACCESS_MASK cmeta_fs_root_access_win(int flags) {
  const int access =
      flags & (SALTS_FS_O_RDONLY | SALTS_FS_O_WRONLY | SALTS_FS_O_RDWR);
  ACCESS_MASK desired = 0u;

  if (access == SALTS_FS_O_RDONLY || access == SALTS_FS_O_RDWR)
    desired |= FILE_READ_DATA | FILE_READ_EA;
  if (access == SALTS_FS_O_WRONLY || access == SALTS_FS_O_RDWR)
    desired |= (ACCESS_MASK)(FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA);
  return desired;
}

static ULONG cmeta_fs_root_disposition_win(int flags) {
  if (flags & SALTS_FS_ROOT_O_EXCL) return FILE_CREATE;
  if ((flags & SALTS_FS_O_CREAT) != 0) {
    return (flags & SALTS_FS_O_TRUNC) != 0
        ? FILE_OVERWRITE_IF : FILE_OPEN_IF;
  }
  return (flags & SALTS_FS_O_TRUNC) != 0
      ? FILE_OVERWRITE : FILE_OPEN;
}

static int cmeta_fs_root_open_path_win(
    const cmeta_fs_root_t *root,
    const char *path,
    ACCESS_MASK access,
    ULONG disposition,
    ULONG options,
    ULONG attributes,
    bool allow_reparse,
    HANDLE *out_handle) {
  HANDLE parent = INVALID_HANDLE_VALUE;
  bool parent_owned = false;
  char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
  int rc = cmeta_fs_root_open_parent_win(
      root, path, &parent, &parent_owned, final_component);
  if (rc == 0)
    rc = cmeta_fs_root_open_component_win(
        parent, final_component, access, disposition, options,
        attributes, allow_reparse, out_handle);
  if (parent_owned) CloseHandle(parent);
  return rc;
}

static int cmeta_fs_root_delete_handle_win(HANDLE handle) {
  FILE_DISPOSITION_INFO disposition;
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(
          handle, FileDispositionInfo, &disposition, sizeof(disposition)))
    return cmeta_fs_root_win32_error(GetLastError());
  return 0;
}

static bool cmeta_fs_root_dir_name_is_dot_win(
    const FILE_FULL_DIR_INFO *info) {
  size_t chars;
  if (!info) return false;
  chars = info->FileNameLength / sizeof(WCHAR);
  return (chars == 1u && info->FileName[0] == L'.') ||
         (chars == 2u && info->FileName[0] == L'.' &&
          info->FileName[1] == L'.');
}

static int cmeta_fs_root_dir_fill_win(
    cmeta_fs_root_dir_t *dir, bool restart) {
  FILE_INFO_BY_HANDLE_CLASS info_class =
      restart ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo;
  if (!GetFileInformationByHandleEx(
          dir->handle, info_class, dir->buffer, sizeof(dir->buffer))) {
    DWORD error = GetLastError();
    if (error == ERROR_NO_MORE_FILES) {
      dir->have_buffer = false;
      return 0;
    }
    return cmeta_fs_root_win32_error(error);
  }
  dir->offset = 0u;
  dir->have_buffer = true;
  dir->restart = false;
  return 1;
}

static int cmeta_fs_root_dir_next_win(
    cmeta_fs_root_dir_t *dir,
    const FILE_FULL_DIR_INFO **out_info) {
  if (!dir || !out_info)
    return -EINVAL;
  for (;;) {
    const FILE_FULL_DIR_INFO *info;
    if (!dir->have_buffer) {
      int fill = cmeta_fs_root_dir_fill_win(dir, dir->restart);
      if (fill <= 0) return fill;
    }
    info = (const FILE_FULL_DIR_INFO *)(const void *)
        (dir->buffer + dir->offset);
    if (info->NextEntryOffset == 0u) {
      dir->have_buffer = false;
      dir->offset = 0u;
    } else {
      if (info->NextEntryOffset >= sizeof(dir->buffer) ||
          dir->offset > sizeof(dir->buffer) - info->NextEntryOffset)
        return -EIO;
      dir->offset += info->NextEntryOffset;
    }
    if (cmeta_fs_root_dir_name_is_dot_win(info))
      continue;
    *out_info = info;
    return 1;
  }
}

static int cmeta_fs_root_dir_seek_cookie_win(
    cmeta_fs_root_dir_t *dir, uint64_t cookie) {
  if (!dir) return -EINVAL;
  if (cookie == dir->cookie)
    return 0;
  dir->cookie = 0u;
  dir->have_buffer = false;
  dir->offset = 0u;
  dir->restart = true;
  while (dir->cookie < cookie) {
    const FILE_FULL_DIR_INFO *info = NULL;
    int rc = cmeta_fs_root_dir_next_win(dir, &info);
    if (rc <= 0)
      return rc == 0 ? -EINVAL : rc;
    ++dir->cookie;
  }
  return 0;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct cmeta_fs_root_s {
  int fd;
};

struct cmeta_fs_root_file_s {
  int fd;
};

struct cmeta_fs_root_dir_s {
  DIR *stream;
  uint64_t cookie;
};

static int cmeta_fs_root_dup_dir_fd(int fd) {
#ifdef F_DUPFD_CLOEXEC
  return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
  int duplicate = dup(fd);
  if (duplicate >= 0)
    (void)fcntl(duplicate, F_SETFD, FD_CLOEXEC);
  return duplicate;
#endif
}

static int cmeta_fs_root_open_parent_posix(
    const cmeta_fs_root_t *root,
    const char *path,
    int *out_parent,
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX]) {
  cmeta_fs_root_path_cursor_t cursor = {path, 0u, 0u};
  int current;

  if (!root || root->fd < 0 || !path || !out_parent || !final_component)
    return -EINVAL;
  {
    int rc = cmeta_fs_root_validate_relative_path(path);
    if (rc != 0)
      return rc;
  }
  current = cmeta_fs_root_dup_dir_fd(root->fd);
  if (current < 0)
    return -errno;

  for (;;) {
    char component[SALTS_FS_ROOT_COMPONENT_MAX];
    bool last = false;
    int rc = cmeta_fs_root_next_component(&cursor, component, &last);
    if (rc != 0) {
      close(current);
      return rc;
    }
    if (last) {
      memcpy(final_component, component, strlen(component) + 1u);
      *out_parent = current;
      return 0;
    }

    int next = openat(
        current, component,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      rc = -errno;
      close(current);
      return rc;
    }
    close(current);
    current = next;
  }
}

static int cmeta_fs_root_flags_posix(int flags, int *out_flags) {
  const int access =
      flags & (SALTS_FS_O_RDONLY | SALTS_FS_O_WRONLY | SALTS_FS_O_RDWR);
  int native = 0;
  int rc = cmeta_fs_root_validate_open_flags(flags);
  if (rc != 0 || !out_flags)
    return rc != 0 ? rc : -EINVAL;
  if (access == SALTS_FS_O_RDONLY) native |= O_RDONLY;
  else if (access == SALTS_FS_O_WRONLY) native |= O_WRONLY;
  else native |= O_RDWR;
  if ((flags & SALTS_FS_O_CREAT) != 0) native |= O_CREAT;
  if ((flags & SALTS_FS_O_TRUNC) != 0) native |= O_TRUNC;
  if ((flags & SALTS_FS_O_APPEND) != 0) native |= O_APPEND;
  if ((flags & SALTS_FS_ROOT_O_EXCL) != 0) native |= O_EXCL;
  native |= O_NOFOLLOW | O_CLOEXEC;
  *out_flags = native;
  return 0;
}

static void cmeta_fs_root_stat_native_posix(
    const struct stat *native, cmeta_fs_stat_t *out_stat) {
  memset(out_stat, 0, sizeof(*out_stat));
  out_stat->size = (uint64_t)native->st_size;
  out_stat->mode = (int)native->st_mode;
  out_stat->is_file = S_ISREG(native->st_mode) != 0;
  out_stat->is_directory = S_ISDIR(native->st_mode) != 0;
  out_stat->is_symlink = S_ISLNK(native->st_mode) != 0;
#ifdef __APPLE__
  out_stat->atime =
      (uint64_t)native->st_atimespec.tv_sec * 1000000ULL +
      (uint64_t)native->st_atimespec.tv_nsec / 1000ULL;
  out_stat->mtime =
      (uint64_t)native->st_mtimespec.tv_sec * 1000000ULL +
      (uint64_t)native->st_mtimespec.tv_nsec / 1000ULL;
  out_stat->ctime =
      (uint64_t)native->st_ctimespec.tv_sec * 1000000ULL +
      (uint64_t)native->st_ctimespec.tv_nsec / 1000ULL;
#else
  out_stat->atime =
      (uint64_t)native->st_atim.tv_sec * 1000000ULL +
      (uint64_t)native->st_atim.tv_nsec / 1000ULL;
  out_stat->mtime =
      (uint64_t)native->st_mtim.tv_sec * 1000000ULL +
      (uint64_t)native->st_mtim.tv_nsec / 1000ULL;
  out_stat->ctime =
      (uint64_t)native->st_ctim.tv_sec * 1000000ULL +
      (uint64_t)native->st_ctim.tv_nsec / 1000ULL;
#endif
}

static cmeta_fs_dirent_type_t cmeta_fs_root_dirent_type_posix(
    unsigned char type) {
#ifdef DT_REG
  if (type == DT_REG) return SALTS_FS_DIRENT_FILE;
#endif
#ifdef DT_DIR
  if (type == DT_DIR) return SALTS_FS_DIRENT_DIRECTORY;
#endif
#ifdef DT_LNK
  if (type == DT_LNK) return SALTS_FS_DIRENT_SYMLINK;
#endif
#ifdef DT_UNKNOWN
  if (type == DT_UNKNOWN) return SALTS_FS_DIRENT_UNKNOWN;
#endif
  return SALTS_FS_DIRENT_OTHER;
}

static bool cmeta_fs_root_dir_name_is_dot_posix(const char *name) {
  return name && name[0] == '.' &&
      (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int cmeta_fs_root_dir_next_posix(
    cmeta_fs_root_dir_t *dir, const struct dirent **out_entry) {
  if (!dir || !dir->stream || !out_entry)
    return -EINVAL;
  for (;;) {
    struct dirent *entry;
    errno = 0;
    entry = readdir(dir->stream);
    if (!entry)
      return errno == 0 ? 0 : -errno;
    if (cmeta_fs_root_dir_name_is_dot_posix(entry->d_name))
      continue;
    *out_entry = entry;
    return 1;
  }
}

static int cmeta_fs_root_dir_seek_cookie_posix(
    cmeta_fs_root_dir_t *dir, uint64_t cookie) {
  if (!dir || !dir->stream)
    return -EINVAL;
  if (cookie == dir->cookie)
    return 0;
  rewinddir(dir->stream);
  dir->cookie = 0u;
  while (dir->cookie < cookie) {
    const struct dirent *entry = NULL;
    int rc = cmeta_fs_root_dir_next_posix(dir, &entry);
    if (rc <= 0)
      return rc == 0 ? -EINVAL : rc;
    ++dir->cookie;
  }
  return 0;
}

#endif

int cmeta_fs_root_open(
    const char *host_root, cmeta_fs_root_t **out_root) {
  cmeta_fs_root_t *root;
  if (!out_root)
    return -EINVAL;
  *out_root = NULL;
  if (!cmeta_fs_root_host_path_is_absolute(host_root))
    return -EINVAL;

  root = (cmeta_fs_root_t *)calloc(1, sizeof(*root));
  if (!root)
    return -ENOMEM;

#ifdef _WIN32
  {
    int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, host_root, -1, NULL, 0);
    WCHAR *wide;
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (needed <= 0 || needed > 32767) {
      free(root);
      return needed <= 0
          ? cmeta_fs_root_win32_error(GetLastError())
          : -ENAMETOOLONG;
    }
    wide = (WCHAR *)calloc((size_t)needed, sizeof(WCHAR));
    if (!wide) {
      free(root);
      return -ENOMEM;
    }
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, host_root, -1,
            wide, needed) <= 0) {
      int rc = cmeta_fs_root_win32_error(GetLastError());
      free(wide);
      free(root);
      return rc;
    }
    root->handle = CreateFileW(
        wide,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (root->handle == INVALID_HANDLE_VALUE) {
      int rc = cmeta_fs_root_win32_error(GetLastError());
      free(root);
      return rc;
    }
    if (!GetFileInformationByHandleEx(
            root->handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
      int rc = cmeta_fs_root_win32_error(GetLastError());
      CloseHandle(root->handle);
      free(root);
      return rc;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
      CloseHandle(root->handle);
      free(root);
      return -ENOTDIR;
    }
  }
#else
  root->fd = open(host_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root->fd < 0) {
    int rc = -errno;
    free(root);
    return rc;
  }
#endif

  *out_root = root;
  return 0;
}

int cmeta_fs_root_close(cmeta_fs_root_t *root) {
  if (!root)
    return -EINVAL;
#ifdef _WIN32
  if (root->handle == INVALID_HANDLE_VALUE) {
    free(root);
    return -EINVAL;
  }
  if (!CloseHandle(root->handle)) {
    int rc = cmeta_fs_root_win32_error(GetLastError());
    free(root);
    return rc;
  }
#else
  if (root->fd < 0) {
    free(root);
    return -EINVAL;
  }
  if (close(root->fd) != 0) {
    int rc = -errno;
    free(root);
    return rc;
  }
#endif
  free(root);
  return 0;
}

int cmeta_fs_root_fstat(
    const cmeta_fs_root_t *root, cmeta_fs_stat_t *out_stat) {
  if (!root || !out_stat)
    return -EINVAL;
#ifdef _WIN32
  return cmeta_fs_root_stat_handle_win(root->handle, out_stat);
#else
  {
    struct stat native;
    if (fstat(root->fd, &native) != 0)
      return -errno;
    cmeta_fs_root_stat_native_posix(&native, out_stat);
    return 0;
  }
#endif
}

int cmeta_fs_root_file_open(
    const cmeta_fs_root_t *root, const char *relative_path,
    int flags, int mode, cmeta_fs_root_file_t **out_file) {
  cmeta_fs_root_file_t *file;
  int rc;
  if (!out_file)
    return -EINVAL;
  *out_file = NULL;
  rc = cmeta_fs_root_validate_open_flags(flags);
  if ((flags & SALTS_FS_ROOT_O_EXCL) && !(flags & SALTS_FS_O_CREAT))
    return -EINVAL;
  if (rc != 0)
    return rc;
  if (!root || !relative_path)
    return -EINVAL;

  file = (cmeta_fs_root_file_t *)calloc(1, sizeof(*file));
  if (!file)
    return -ENOMEM;

#ifdef _WIN32
  (void)mode;
  file->handle = INVALID_HANDLE_VALUE;
  file->append = (flags & SALTS_FS_O_APPEND) != 0;
  rc = cmeta_fs_root_open_path_win(
      root, relative_path,
      cmeta_fs_root_access_win(flags),
      cmeta_fs_root_disposition_win(flags),
      FILE_NON_DIRECTORY_FILE,
      FILE_ATTRIBUTE_NORMAL,
      false,
      &file->handle);
#else
  {
    int parent = -1;
    int native_flags = 0;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    file->fd = -1;
    rc = cmeta_fs_root_flags_posix(flags, &native_flags);
    if (rc == 0)
      rc = cmeta_fs_root_open_parent_posix(
          root, relative_path, &parent, final_component);
    if (rc == 0) {
      file->fd = openat(parent, final_component, native_flags, mode);
      if (file->fd < 0)
        rc = -errno;
    }
    if (parent >= 0)
      close(parent);
  }
#endif
  if (rc != 0) {
    free(file);
    return rc;
  }
  *out_file = file;
  return 0;
}

int cmeta_fs_root_file_close(cmeta_fs_root_file_t *file) {
  if (!file)
    return -EINVAL;
#ifdef _WIN32
  if (file->handle == INVALID_HANDLE_VALUE) {
    free(file);
    return -EINVAL;
  }
  if (!CloseHandle(file->handle)) {
    int rc = cmeta_fs_root_win32_error(GetLastError());
    free(file);
    return rc;
  }
#else
  if (file->fd < 0) {
    free(file);
    return -EINVAL;
  }
  if (close(file->fd) != 0) {
    int rc = -errno;
    free(file);
    return rc;
  }
#endif
  free(file);
  return 0;
}

int cmeta_fs_root_file_read(
    cmeta_fs_root_file_t *file, char *buffer, size_t length) {
  if (!file || (!buffer && length != 0u) || length > (size_t)INT_MAX)
    return length > (size_t)INT_MAX ? -EOVERFLOW : -EINVAL;
  if (length == 0u)
    return 0;
#ifdef _WIN32
  {
    DWORD count = 0u;
    if (!ReadFile(file->handle, buffer, (DWORD)length, &count, NULL))
      return cmeta_fs_root_win32_error(GetLastError());
    return (int)count;
  }
#else
  for (;;) {
    ssize_t count = read(file->fd, buffer, length);
    if (count < 0 && errno == EINTR) continue;
    return count < 0 ? -errno : (int)count;
  }
#endif
}

int cmeta_fs_root_file_write(
    cmeta_fs_root_file_t *file, const char *data, size_t length) {
  if (!file || (!data && length != 0u) || length > (size_t)INT_MAX)
    return length > (size_t)INT_MAX ? -EOVERFLOW : -EINVAL;
  if (length == 0u)
    return 0;
#ifdef _WIN32
  {
    DWORD count = 0u;
    OVERLAPPED position = {0};
    position.Offset = MAXDWORD;
    position.OffsetHigh = MAXDWORD;
    if (!WriteFile(file->handle, data, (DWORD)length, &count,
                   file->append ? &position : NULL))
      return cmeta_fs_root_win32_error(GetLastError());
    return (int)count;
  }
#else
  for (;;) {
    ssize_t count = write(file->fd, data, length);
    if (count < 0 && errno == EINTR) continue;
    return count < 0 ? -errno : (int)count;
  }
#endif
}

int64_t cmeta_fs_root_file_seek(
    cmeta_fs_root_file_t *file, int64_t offset, int whence) {
  if (!file)
    return -EINVAL;
#ifdef _WIN32
  {
    LARGE_INTEGER distance;
    LARGE_INTEGER result;
    DWORD method;
    if (whence == SEEK_SET) method = FILE_BEGIN;
    else if (whence == SEEK_CUR) method = FILE_CURRENT;
    else if (whence == SEEK_END) method = FILE_END;
    else return -EINVAL;
    distance.QuadPart = offset;
    if (!SetFilePointerEx(file->handle, distance, &result, method))
      return cmeta_fs_root_win32_error(GetLastError());
    return result.QuadPart;
  }
#else
  {
    off_t native = (off_t)offset;
    off_t result;
    if ((int64_t)native != offset)
      return -EOVERFLOW;
    result = lseek(file->fd, native, whence);
    return result < 0 ? (int64_t)-errno : (int64_t)result;
  }
#endif
}

int64_t cmeta_fs_root_file_tell(cmeta_fs_root_file_t *file) {
  return cmeta_fs_root_file_seek(file, 0, SEEK_CUR);
}

int cmeta_fs_root_file_set_append(cmeta_fs_root_file_t *file, bool append) {
  if (!file) return -EINVAL;
#ifdef _WIN32
  file->append = append;
  return 0;
#else
  int flags = fcntl(file->fd, F_GETFL);
  if (flags < 0) return -errno;
  int updated = append ? (flags | O_APPEND) : (flags & ~O_APPEND);
  return fcntl(file->fd, F_SETFL, updated) < 0 ? -errno : 0;
#endif
}

int cmeta_fs_root_rename(
    const cmeta_fs_root_t *source_root, const char *source_path,
    const cmeta_fs_root_t *target_root, const char *target_path) {
  char source_name[SALTS_FS_ROOT_COMPONENT_MAX];
  char target_name[SALTS_FS_ROOT_COMPONENT_MAX];
#ifdef _WIN32
  HANDLE source_parent = INVALID_HANDLE_VALUE, target_parent = INVALID_HANDLE_VALUE;
  HANDLE source = INVALID_HANDLE_VALUE;
  bool source_owned = false, target_owned = false;
  int rc = cmeta_fs_root_open_parent_win(source_root, source_path,
      &source_parent, &source_owned, source_name);
  if (!rc) rc = cmeta_fs_root_open_parent_win(target_root, target_path,
      &target_parent, &target_owned, target_name);
  if (!rc) rc = cmeta_fs_root_open_component_win(source_parent, source_name,
      DELETE | SYNCHRONIZE, FILE_OPEN, 0, FILE_ATTRIBUTE_NORMAL, true, &source);
  if (!rc) {
    WCHAR wide[SALTS_FS_ROOT_COMPONENT_MAX];
    USHORT length = 0;
    rc = cmeta_fs_root_utf8_component_to_wide(target_name, wide, &length);
    if (!rc) {
      size_t size = offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(WCHAR);
      FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)calloc(1, size);
      if (!info) rc = -ENOMEM;
      else {
        info->ReplaceIfExists = TRUE;
        info->RootDirectory = target_parent;
        info->FileNameLength = length;
        memcpy(info->FileName, wide, length);
        /* Preserve the admitted parent HANDLE all the way to the NT operation.
         * Win32 path normalization is not part of this capability boundary. */
        IO_STATUS_BLOCK io = {0};
        NTSTATUS status = cmeta_fs_root_nt_set_information(source, &io, info,
            (ULONG)size, (FILE_INFORMATION_CLASS)CMETA_FS_FILE_RENAME_INFORMATION);
        if (status < 0) rc = cmeta_fs_root_nt_error(status);
        free(info);
      }
    }
  }
  if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
  if (source_owned) CloseHandle(source_parent);
  if (target_owned) CloseHandle(target_parent);
  return rc;
#else
  int source_parent = -1, target_parent = -1;
  int rc = cmeta_fs_root_open_parent_posix(source_root, source_path,
      &source_parent, source_name);
  if (!rc) rc = cmeta_fs_root_open_parent_posix(target_root, target_path,
      &target_parent, target_name);
  if (!rc && renameat(source_parent, source_name, target_parent, target_name))
    rc = -errno;
  if (source_parent >= 0) close(source_parent);
  if (target_parent >= 0) close(target_parent);
  return rc;
#endif
}

int cmeta_fs_root_file_stat(
    const cmeta_fs_root_file_t *file, cmeta_fs_stat_t *out_stat) {
  if (!file || !out_stat)
    return -EINVAL;
#ifdef _WIN32
  return cmeta_fs_root_stat_handle_win(file->handle, out_stat);
#else
  {
    struct stat native;
    if (fstat(file->fd, &native) != 0)
      return -errno;
    cmeta_fs_root_stat_native_posix(&native, out_stat);
    return 0;
  }
#endif
}

static int cmeta_fs_root_path_stat_impl(
    const cmeta_fs_root_t *root,
    const char *relative_path,
    bool allow_final_link,
    cmeta_fs_stat_t *out_stat) {
  if (!root || !relative_path || !out_stat)
    return -EINVAL;
#ifdef _WIN32
  {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int rc = cmeta_fs_root_open_path_win(
        root, relative_path,
        FILE_READ_ATTRIBUTES,
        FILE_OPEN, 0u, FILE_ATTRIBUTE_NORMAL,
        true, &handle);
    if (rc != 0)
      return rc;
    rc = cmeta_fs_root_stat_handle_win(handle, out_stat);
    CloseHandle(handle);
    if (rc != 0)
      return rc;
    if (!allow_final_link && out_stat->is_symlink)
      return -ELOOP;
    return 0;
  }
#else
  {
    int parent = -1;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    struct stat native;
    int rc = cmeta_fs_root_open_parent_posix(
        root, relative_path, &parent, final_component);
    if (rc != 0)
      return rc;
    if (fstatat(parent, final_component, &native, AT_SYMLINK_NOFOLLOW) != 0)
      rc = -errno;
    close(parent);
    if (rc != 0)
      return rc;
    cmeta_fs_root_stat_native_posix(&native, out_stat);
    if (!allow_final_link && out_stat->is_symlink)
      return -ELOOP;
    return 0;
  }
#endif
}

int cmeta_fs_root_stat(
    const cmeta_fs_root_t *root, const char *relative_path,
    cmeta_fs_stat_t *out_stat) {
  return cmeta_fs_root_path_stat_impl(
      root, relative_path, false, out_stat);
}

int cmeta_fs_root_lstat(
    const cmeta_fs_root_t *root, const char *relative_path,
    cmeta_fs_stat_t *out_stat) {
  return cmeta_fs_root_path_stat_impl(
      root, relative_path, true, out_stat);
}

int cmeta_fs_root_mkdir(
    const cmeta_fs_root_t *root, const char *relative_path, int mode) {
  if (!root || !relative_path)
    return -EINVAL;
#ifdef _WIN32
  {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int rc;
    (void)mode;
    rc = cmeta_fs_root_open_path_win(
        root, relative_path,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE,
        FILE_CREATE, FILE_DIRECTORY_FILE,
        FILE_ATTRIBUTE_DIRECTORY, false, &handle);
    if (rc == 0)
      CloseHandle(handle);
    return rc;
  }
#else
  {
    int parent = -1;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    int rc = cmeta_fs_root_open_parent_posix(
        root, relative_path, &parent, final_component);
    if (rc == 0 && mkdirat(parent, final_component, (mode_t)mode) != 0)
      rc = -errno;
    if (parent >= 0) close(parent);
    return rc;
  }
#endif
}

int cmeta_fs_root_rmdir(
    const cmeta_fs_root_t *root, const char *relative_path) {
  if (!root || !relative_path)
    return -EINVAL;
#ifdef _WIN32
  {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int rc = cmeta_fs_root_open_path_win(
        root, relative_path, DELETE,
        FILE_OPEN, FILE_DIRECTORY_FILE,
        FILE_ATTRIBUTE_NORMAL, false, &handle);
    if (rc == 0) {
      rc = cmeta_fs_root_delete_handle_win(handle);
      CloseHandle(handle);
    }
    return rc;
  }
#else
  {
    int parent = -1;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    int rc = cmeta_fs_root_open_parent_posix(
        root, relative_path, &parent, final_component);
    if (rc == 0 &&
        unlinkat(parent, final_component, AT_REMOVEDIR) != 0)
      rc = -errno;
    if (parent >= 0) close(parent);
    return rc;
  }
#endif
}

int cmeta_fs_root_unlink(
    const cmeta_fs_root_t *root, const char *relative_path) {
  if (!root || !relative_path)
    return -EINVAL;
#ifdef _WIN32
  {
    HANDLE handle = INVALID_HANDLE_VALUE;
    cmeta_fs_stat_t stat_value;
    int rc = cmeta_fs_root_open_path_win(
        root, relative_path, DELETE,
        FILE_OPEN, 0u, FILE_ATTRIBUTE_NORMAL, true, &handle);
    if (rc != 0)
      return rc;
    rc = cmeta_fs_root_stat_handle_win(handle, &stat_value);
    if (rc == 0 && stat_value.is_directory && !stat_value.is_symlink)
      rc = -EISDIR;
    if (rc == 0)
      rc = cmeta_fs_root_delete_handle_win(handle);
    CloseHandle(handle);
    return rc;
  }
#else
  {
    int parent = -1;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    int rc = cmeta_fs_root_open_parent_posix(
        root, relative_path, &parent, final_component);
    if (rc == 0 && unlinkat(parent, final_component, 0) != 0)
      rc = -errno;
    if (parent >= 0) close(parent);
    return rc;
  }
#endif
}

int cmeta_fs_root_opendir_self(
    const cmeta_fs_root_t *root, cmeta_fs_root_dir_t **out_dir) {
  cmeta_fs_root_dir_t *dir;
  if (!out_dir)
    return -EINVAL;
  *out_dir = NULL;
  if (!root)
    return -EINVAL;

  dir = (cmeta_fs_root_dir_t *)calloc(1, sizeof(*dir));
  if (!dir)
    return -ENOMEM;

#ifdef _WIN32
  dir->handle = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(
          GetCurrentProcess(), root->handle,
          GetCurrentProcess(), &dir->handle,
          0u, FALSE, DUPLICATE_SAME_ACCESS)) {
    int rc = cmeta_fs_root_win32_error(GetLastError());
    free(dir);
    return rc;
  }
  dir->restart = true;
#else
  {
    /* dup() shares the directory offset. Each stream must start independently
     * while remaining anchored to the admitted root, even after a rename. */
    int fd = openat(
        root->fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      int rc = -errno;
      free(dir);
      return rc;
    }
    dir->stream = fdopendir(fd);
    if (!dir->stream) {
      int rc = -errno;
      close(fd);
      free(dir);
      return rc;
    }
  }
#endif
  *out_dir = dir;
  return 0;
}

int cmeta_fs_root_opendir(
    const cmeta_fs_root_t *root, const char *relative_path,
    cmeta_fs_root_dir_t **out_dir) {
  cmeta_fs_root_dir_t *dir;
  int rc = 0;
  if (!out_dir)
    return -EINVAL;
  *out_dir = NULL;
  if (!root || !relative_path)
    return -EINVAL;

  dir = (cmeta_fs_root_dir_t *)calloc(1, sizeof(*dir));
  if (!dir)
    return -ENOMEM;

#ifdef _WIN32
  dir->handle = INVALID_HANDLE_VALUE;
  rc = cmeta_fs_root_open_path_win(
      root, relative_path,
      FILE_LIST_DIRECTORY | FILE_TRAVERSE,
      FILE_OPEN, FILE_DIRECTORY_FILE,
      FILE_ATTRIBUTE_NORMAL, false, &dir->handle);
  if (rc == 0)
    dir->restart = true;
#else
  {
    int parent = -1;
    int fd = -1;
    char final_component[SALTS_FS_ROOT_COMPONENT_MAX];
    rc = cmeta_fs_root_open_parent_posix(
        root, relative_path, &parent, final_component);
    if (rc == 0) {
      fd = openat(
          parent, final_component,
          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
        rc = -errno;
    }
    if (parent >= 0) close(parent);
    if (rc == 0) {
      dir->stream = fdopendir(fd);
      if (!dir->stream) {
        rc = -errno;
        close(fd);
      }
    }
  }
#endif

  if (rc != 0) {
    free(dir);
    return rc;
  }
  *out_dir = dir;
  return 0;
}

int cmeta_fs_root_readdir(
    cmeta_fs_root_dir_t *dir, uint64_t cookie,
    char *name_buffer, size_t name_buffer_size,
    cmeta_fs_root_dirent_t *out_entry) {
  int rc;
  if (!dir || !name_buffer || name_buffer_size == 0u || !out_entry)
    return -EINVAL;
  memset(out_entry, 0, sizeof(*out_entry));
  name_buffer[0] = '\0';

#ifdef _WIN32
  {
    const FILE_FULL_DIR_INFO *info = NULL;
    int needed;
    rc = cmeta_fs_root_dir_seek_cookie_win(dir, cookie);
    if (rc != 0) return rc;
    rc = cmeta_fs_root_dir_next_win(dir, &info);
    if (rc <= 0) return rc;

    needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)),
        NULL, 0, NULL, NULL);
    if (needed <= 0)
      return cmeta_fs_root_win32_error(GetLastError());
    if ((size_t)needed + 1u > name_buffer_size ||
        (size_t)needed > SALTS_FS_ROOT_DIRENT_NAME_MAX) {
      dir->cookie = UINT64_MAX;
      return -ERANGE;
    }
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS,
            info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)),
            name_buffer, needed, NULL, NULL) != needed) {
      dir->cookie = UINT64_MAX;
      return cmeta_fs_root_win32_error(GetLastError());
    }
    name_buffer[needed] = '\0';
    out_entry->name_length = (size_t)needed;
    if ((info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
      out_entry->type = SALTS_FS_DIRENT_SYMLINK;
    else if ((info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
      out_entry->type = SALTS_FS_DIRENT_DIRECTORY;
    else
      out_entry->type = SALTS_FS_DIRENT_FILE;
  }
#else
  {
    const struct dirent *entry = NULL;
    size_t name_length;
    rc = cmeta_fs_root_dir_seek_cookie_posix(dir, cookie);
    if (rc != 0) return rc;
    rc = cmeta_fs_root_dir_next_posix(dir, &entry);
    if (rc <= 0) return rc;
    name_length = strlen(entry->d_name);
    if (name_length + 1u > name_buffer_size ||
        name_length > SALTS_FS_ROOT_DIRENT_NAME_MAX) {
      dir->cookie = UINT64_MAX;
      return -ERANGE;
    }
    memcpy(name_buffer, entry->d_name, name_length + 1u);
    out_entry->name_length = name_length;
    out_entry->type = cmeta_fs_root_dirent_type_posix(entry->d_type);
  }
#endif

  if (cookie == UINT64_MAX)
    return -EOVERFLOW;
  dir->cookie = cookie + 1u;
  out_entry->next_cookie = dir->cookie;
  return 1;
}

int cmeta_fs_root_closedir(cmeta_fs_root_dir_t *dir) {
  if (!dir)
    return -EINVAL;
#ifdef _WIN32
  if (dir->handle == INVALID_HANDLE_VALUE) {
    free(dir);
    return -EINVAL;
  }
  if (!CloseHandle(dir->handle)) {
    int rc = cmeta_fs_root_win32_error(GetLastError());
    free(dir);
    return rc;
  }
#else
  if (!dir->stream) {
    free(dir);
    return -EINVAL;
  }
  if (closedir(dir->stream) != 0) {
    int rc = -errno;
    free(dir);
    return rc;
  }
#endif
  free(dir);
  return 0;
}
