/**
 * @file test_cmeta_fs_root.c
 * @brief Security and lifecycle tests for root-relative filesystem capabilities.
 */
#include "tinytest.h"
#include "cmeta_fs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef ELOOP
#define ELOOP EINVAL
#endif

static char root_path[1024];
static char moved_path[1024];
static char outside_dir[1024];
static char outside_file[1024];

static int join_path(
    char *out, size_t out_size, const char *base, const char *child) {
  return cmeta_fs_path_join(out, out_size, base, child);
}

static void remove_known_tree(const char *base) {
  char path[1024];
  char nested[1024];

  if (!base || base[0] == '\0') return;

  if (join_path(path, sizeof(path), base, "data.txt") == 0)
    (void)cmeta_fs_unlink(path);
  if (join_path(path, sizeof(path), base, "append.txt") == 0)
    (void)cmeta_fs_unlink(path);
  if (join_path(path, sizeof(path), base, "inside.txt") == 0)
    (void)cmeta_fs_unlink(path);
  if (join_path(path, sizeof(path), base, "link-file") == 0) {
    (void)cmeta_fs_unlink(path);
    (void)cmeta_fs_rmdir(path);
  }
  if (join_path(path, sizeof(path), base, "jump") == 0) {
    (void)cmeta_fs_unlink(path);
    (void)cmeta_fs_rmdir(path);
  }
  if (join_path(nested, sizeof(nested), base, "nested") == 0) {
    if (join_path(path, sizeof(path), nested, "data.txt") == 0)
      (void)cmeta_fs_unlink(path);
    if (join_path(path, sizeof(path), nested, "a.txt") == 0)
      (void)cmeta_fs_unlink(path);
    if (join_path(path, sizeof(path), nested, "b.txt") == 0)
      (void)cmeta_fs_unlink(path);
    if (join_path(path, sizeof(path), nested, "child") == 0)
      (void)cmeta_fs_rmdir(path);
    (void)cmeta_fs_rmdir(nested);
  }
  (void)cmeta_fs_rmdir(base);
}

static void reset_fixture(void) {
  remove_known_tree(root_path);
  remove_known_tree(moved_path);
  (void)cmeta_fs_unlink(outside_file);
  remove_known_tree(outside_dir);
}

static int write_ambient(const char *path, const char *text) {
  cmeta_fs_buf_t buffer =
      cmeta_fs_buf_init((char *)text, strlen(text));
  return cmeta_fs_write_file(path, &buffer);
}

spec("Salts secure root-relative FS") {
  before_all() {
    char temp[768];
    check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
    check_equal(join_path(
        root_path, sizeof(root_path), temp, "cmeta_fs_root_cap_test"), 0);
    check_equal(join_path(
        moved_path, sizeof(moved_path), temp, "cmeta_fs_root_cap_moved"), 0);
    check_equal(join_path(
        outside_dir, sizeof(outside_dir), temp, "cmeta_fs_root_cap_outside"), 0);
    check_equal(join_path(
        outside_file, sizeof(outside_file), temp,
        "cmeta_fs_root_cap_outside.txt"), 0);
    reset_fixture();
  }

  after_all() {
    reset_fixture();
  }

  group("independent root directory streams") {
    static cmeta_fs_root_t *root;
    static cmeta_fs_root_dir_t *first;
    static cmeta_fs_root_dir_t *second;

    before_each() {
      char path[1024];
      reset_fixture();
      check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
      check_equal(join_path(path, sizeof(path), root_path, "data.txt"), 0);
      check_equal(write_ambient(path, "data"), 0);
      check_equal(join_path(path, sizeof(path), root_path, "append.txt"), 0);
      check_equal(write_ambient(path, "append"), 0);
      check_equal(cmeta_fs_root_open(root_path, &root), 0);
    }

    after_each() {
      if (first) (void)cmeta_fs_root_closedir(first);
      if (second) (void)cmeta_fs_root_closedir(second);
      first = second = NULL;
      if (root) (void)cmeta_fs_root_close(root);
      root = NULL;
      reset_fixture();
    }

    it("reopens at the beginning after an interrupted enumeration") {
      char name[64];
      cmeta_fs_root_dirent_t entry;
      uint64_t cookie = 0;
      size_t count = 0;
      int rc;
      check_equal(cmeta_fs_root_opendir_self(root, &first), 0);
      check_equal(cmeta_fs_root_readdir(first, 0, name, sizeof(name), &entry), 1);
      check_equal(cmeta_fs_root_closedir(first), 0);
      first = NULL;
      check_equal(cmeta_fs_root_opendir_self(root, &second), 0);
      while ((rc = cmeta_fs_root_readdir(second, cookie, name, sizeof(name), &entry)) > 0) {
        check_true(strcmp(name, "data.txt") == 0 || strcmp(name, "append.txt") == 0);
        cookie = entry.next_cookie;
        ++count;
      }
      check_equal(rc, 0);
      check_equal(count, 2u);
    }

    it("keeps interleaved enumeration cursors independent") {
      char first_name[64], second_name[64];
      cmeta_fs_root_dirent_t first_entry, second_entry;
      check_equal(cmeta_fs_root_opendir_self(root, &first), 0);
      check_equal(cmeta_fs_root_opendir_self(root, &second), 0);
      check_equal(cmeta_fs_root_readdir(first, 0, first_name, sizeof(first_name), &first_entry), 1);
      check_equal(cmeta_fs_root_readdir(second, 0, second_name, sizeof(second_name), &second_entry), 1);
      check_equal(cmeta_fs_root_readdir(first, first_entry.next_cookie,
          first_name, sizeof(first_name), &first_entry), 1);
      check_equal(cmeta_fs_root_readdir(first, first_entry.next_cookie,
          first_name, sizeof(first_name), &first_entry), 0);
      check_equal(cmeta_fs_root_readdir(second, second_entry.next_cookie,
          second_name, sizeof(second_name), &second_entry), 1);
      check_equal(cmeta_fs_root_readdir(second, second_entry.next_cookie,
          second_name, sizeof(second_name), &second_entry), 0);
    }

    it("reopens the admitted directory after its path is renamed") {
      char name[64];
      cmeta_fs_root_dirent_t entry;
      check_equal(cmeta_fs_rename(root_path, moved_path), 0);
      check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
      check_equal(cmeta_fs_root_opendir_self(root, &first), 0);
      check_equal(cmeta_fs_root_readdir(first, 0, name, sizeof(name), &entry), 1);
      check_true(strcmp(name, "data.txt") == 0 || strcmp(name, "append.txt") == 0);
    }
  }

  it("admits an absolute root and rejects guest escape syntax") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_file_t *file = NULL;
    cmeta_fs_stat_t stat_value = {0};

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(cmeta_fs_root_open(root_path, &root), 0);
    check_not_null(root);
    check_equal(cmeta_fs_root_fstat(root, &stat_value), 0);
    check_true(stat_value.is_directory);

    check_equal(
        cmeta_fs_root_file_open(
            root, "../outside", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "nested/../outside", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "/absolute", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "\\absolute", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "./file", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "nested//file", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
    check_equal(
        cmeta_fs_root_file_open(
            root, "nested/", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
#ifdef _WIN32
    check_equal(
        cmeta_fs_root_file_open(
            root, "file:stream", SALTS_FS_O_RDONLY, 0, &file),
        -EINVAL);
    check_null(file);
#endif

    check_equal(cmeta_fs_root_close(root), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
  }

  it("performs file and directory operations relative to the capability") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_file_t *file = NULL;
    cmeta_fs_stat_t stat_value = {0};
    char buffer[16] = {0};

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(cmeta_fs_root_open(root_path, &root), 0);
    check_equal(cmeta_fs_root_mkdir(root, "nested", 0755), 0);
    check_equal(
        cmeta_fs_root_file_open(
            root, "nested/data.txt",
            SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC,
            0644, &file),
        0);
    check_equal(cmeta_fs_root_file_write(file, "hello", 5u), 5);
    check_equal(cmeta_fs_root_file_tell(file), (int64_t)5);
    check_equal(cmeta_fs_root_file_stat(file, &stat_value), 0);
    check_equal(stat_value.size, (uint64_t)5);
    check_equal(cmeta_fs_root_file_close(file), 0);
    file = NULL;

    check_equal(
        cmeta_fs_root_file_open(
            root, "nested/data.txt", SALTS_FS_O_RDONLY, 0, &file),
        0);
    check_equal(cmeta_fs_root_file_read(file, buffer, 5u), 5);
    check_equal(buffer, "hello");
    check_equal(cmeta_fs_root_file_seek(file, 1, SEEK_SET), (int64_t)1);
    check_equal(cmeta_fs_root_file_tell(file), (int64_t)1);
    check_equal(cmeta_fs_root_file_close(file), 0);
    file = NULL;

    check_equal(
        cmeta_fs_root_stat(root, "nested/data.txt", &stat_value), 0);
    check_true(stat_value.is_file);
    check_equal(cmeta_fs_root_unlink(root, "nested/data.txt"), 0);
    check_equal(cmeta_fs_root_rmdir(root, "nested"), 0);
    check_equal(cmeta_fs_root_close(root), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
  }

  it("preserves append semantics even after seeking away from EOF") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_file_t *file = NULL;
    char buffer[4] = {0};

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(cmeta_fs_root_open(root_path, &root), 0);

    check_equal(
        cmeta_fs_root_file_open(
            root, "append.txt",
            SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC,
            0644, &file),
        0);
    check_equal(cmeta_fs_root_file_write(file, "a", 1u), 1);
    check_equal(cmeta_fs_root_file_close(file), 0);
    file = NULL;

    check_equal(
        cmeta_fs_root_file_open(
            root, "append.txt",
            SALTS_FS_O_WRONLY | SALTS_FS_O_APPEND,
            0644, &file),
        0);
    check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), (int64_t)0);
    check_equal(cmeta_fs_root_file_write(file, "b", 1u), 1);
    check_equal(cmeta_fs_root_file_close(file), 0);
    file = NULL;

    check_equal(
        cmeta_fs_root_file_open(
            root, "append.txt", SALTS_FS_O_RDONLY, 0, &file),
        0);
    check_equal(cmeta_fs_root_file_read(file, buffer, 2u), 2);
    check_equal(buffer, "ab");
    check_equal(cmeta_fs_root_file_close(file), 0);
    check_equal(cmeta_fs_root_unlink(root, "append.txt"), 0);
    check_equal(cmeta_fs_root_close(root), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
  }

  it("enumerates the root and child directories with caller-owned names") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_dir_t *dir = NULL;
    cmeta_fs_root_dirent_t entry = {0};
    char nested[1024];
    char path[1024];
    char name[64];
    char tiny[2];
    bool saw_a = false;
    bool saw_b = false;
    uint64_t cookie = 0u;
    int rc;

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(join_path(nested, sizeof(nested), root_path, "nested"), 0);
    check_equal(cmeta_fs_mkdir(nested, 0755), 0);
    check_equal(join_path(path, sizeof(path), nested, "a.txt"), 0);
    check_equal(write_ambient(path, "a"), 0);
    check_equal(join_path(path, sizeof(path), nested, "b.txt"), 0);
    check_equal(write_ambient(path, "b"), 0);

    check_equal(cmeta_fs_root_open(root_path, &root), 0);
    check_equal(cmeta_fs_root_opendir_self(root, &dir), 0);
    check_equal(
        cmeta_fs_root_readdir(
            dir, 0u, name, sizeof(name), &entry),
        1);
    check_equal(name, "nested");
    check_equal(cmeta_fs_root_closedir(dir), 0);
    dir = NULL;

    check_equal(cmeta_fs_root_opendir(root, "nested", &dir), 0);
    check_equal(
        cmeta_fs_root_readdir(
            dir, 0u, tiny, sizeof(tiny), &entry),
        -ERANGE);

    while ((rc = cmeta_fs_root_readdir(
                dir, cookie, name, sizeof(name), &entry)) > 0) {
      if (strcmp(name, "a.txt") == 0) saw_a = true;
      if (strcmp(name, "b.txt") == 0) saw_b = true;
      cookie = entry.next_cookie;
    }
    check_equal(rc, 0);
    check_true(saw_a);
    check_true(saw_b);
    check_equal(cmeta_fs_root_closedir(dir), 0);

    check_equal(cmeta_fs_root_unlink(root, "nested/a.txt"), 0);
    check_equal(cmeta_fs_root_unlink(root, "nested/b.txt"), 0);
    check_equal(cmeta_fs_root_rmdir(root, "nested"), 0);
    check_equal(cmeta_fs_root_close(root), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
  }

  it("does not follow final or intermediate symlinks outside the root") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_file_t *file = NULL;
    cmeta_fs_stat_t stat_value = {0};
    char link_file[1024];
    char jump[1024];
    char secret[1024];
    int file_link_rc;
    int dir_link_rc;

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(cmeta_fs_mkdir(outside_dir, 0755), 0);
    check_equal(write_ambient(outside_file, "outside"), 0);
    check_equal(join_path(secret, sizeof(secret), outside_dir, "secret.txt"), 0);
    check_equal(write_ambient(secret, "secret"), 0);
    check_equal(join_path(link_file, sizeof(link_file), root_path, "link-file"), 0);
    check_equal(join_path(jump, sizeof(jump), root_path, "jump"), 0);

    file_link_rc = cmeta_fs_symlink(outside_file, link_file, 0);
    dir_link_rc = cmeta_fs_symlink(outside_dir, jump, 1);

    check_equal(cmeta_fs_root_open(root_path, &root), 0);

    if (file_link_rc == 0) {
      check_equal(
          cmeta_fs_root_lstat(root, "link-file", &stat_value), 0);
      check_true(stat_value.is_symlink);
      check_equal(
          cmeta_fs_root_stat(root, "link-file", &stat_value), -ELOOP);
      check_less(
          cmeta_fs_root_file_open(
              root, "link-file", SALTS_FS_O_RDONLY, 0, &file),
          0);
      check_null(file);
      check_equal(cmeta_fs_root_unlink(root, "link-file"), 0);
      check_equal(cmeta_fs_stat(outside_file, &stat_value), 0);
    }

    if (dir_link_rc == 0) {
      check_less(
          cmeta_fs_root_file_open(
              root, "jump/secret.txt", SALTS_FS_O_RDONLY, 0, &file),
          0);
      check_null(file);
      check_equal(cmeta_fs_root_unlink(root, "jump"), 0);
      check_equal(cmeta_fs_stat(secret, &stat_value), 0);
    }

    check_equal(cmeta_fs_root_close(root), 0);
    (void)cmeta_fs_unlink(link_file);
    (void)cmeta_fs_unlink(jump);
    check_equal(cmeta_fs_unlink(secret), 0);
    check_equal(cmeta_fs_rmdir(outside_dir), 0);
    check_equal(cmeta_fs_unlink(outside_file), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
  }

  it("retains the admitted directory identity across host path replacement") {
    cmeta_fs_root_t *root = NULL;
    cmeta_fs_root_file_t *file = NULL;
    char original_file[1024];
    char replacement_file[1024];
    char buffer[16] = {0};

    reset_fixture();
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(
        join_path(original_file, sizeof(original_file),
                  root_path, "inside.txt"),
        0);
    check_equal(write_ambient(original_file, "original"), 0);

    check_equal(cmeta_fs_root_open(root_path, &root), 0);
    check_equal(cmeta_fs_rename(root_path, moved_path), 0);
    check_equal(cmeta_fs_mkdir(root_path, 0755), 0);
    check_equal(
        join_path(replacement_file, sizeof(replacement_file),
                  root_path, "inside.txt"),
        0);
    check_equal(write_ambient(replacement_file, "attacker"), 0);

    check_equal(
        cmeta_fs_root_file_open(
            root, "inside.txt", SALTS_FS_O_RDONLY, 0, &file),
        0);
    check_equal(cmeta_fs_root_file_read(file, buffer, 8u), 8);
    check_equal(buffer, "original");
    check_equal(cmeta_fs_root_file_close(file), 0);

    check_equal(cmeta_fs_root_close(root), 0);
    check_equal(cmeta_fs_unlink(replacement_file), 0);
    check_equal(cmeta_fs_rmdir(root_path), 0);
    check_equal(
        join_path(original_file, sizeof(original_file),
                  moved_path, "inside.txt"),
        0);
    check_equal(cmeta_fs_unlink(original_file), 0);
    check_equal(cmeta_fs_rmdir(moved_path), 0);
  }

  group("exclusive creation, rename and append mutation") {
    static cmeta_fs_root_t *root;
    static cmeta_fs_root_file_t *file;
    before_each() {
      root = NULL; file = NULL;
      reset_fixture();
      check_equal(cmeta_fs_mkdir(root_path, 0700), 0);
      check_equal(cmeta_fs_root_open(root_path, &root), 0);
    }
    after_each() {
      if (file) { cmeta_fs_root_file_close(file); file = NULL; }
      if (root) { cmeta_fs_root_close(root); root = NULL; }
      reset_fixture();
    }
    it("exclusive creation never truncates an existing file") {
      check_equal(cmeta_fs_root_file_open(root, "data.txt",
          SALTS_FS_O_RDWR | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL, 0600, &file), 0);
      check_equal(cmeta_fs_root_file_write(file, "abc", 3), 3);
      cmeta_fs_root_file_t *other = NULL;
      check_equal(cmeta_fs_root_file_open(root, "data.txt",
          SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC | SALTS_FS_ROOT_O_EXCL,
          0600, &other), -EEXIST);
      check_null(other);
      check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), 0);
      char text[4] = {0};
      check_equal(cmeta_fs_root_file_read(file, text, 3), 3);
      check_equal(text, "abc");
    }
    it("toggles atomic append without reopening or losing file identity") {
      check_equal(cmeta_fs_root_file_open(root, "append.txt",
          SALTS_FS_O_RDWR | SALTS_FS_O_CREAT, 0600, &file), 0);
      check_equal(cmeta_fs_root_file_write(file, "abc", 3), 3);
      check_equal(cmeta_fs_root_file_set_append(file, true), 0);
      check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), 0);
      check_equal(cmeta_fs_root_file_write(file, "d", 1), 1);
      check_equal(cmeta_fs_root_file_set_append(file, false), 0);
      check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), 0);
      check_equal(cmeta_fs_root_file_write(file, "A", 1), 1);
      check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), 0);
      char text[5] = {0};
      check_equal(cmeta_fs_root_file_read(file, text, 4), 4);
      check_equal(text, "Abcd");
    }
    it("renames and replaces entries while preserving open source handles") {
      check_equal(cmeta_fs_root_file_open(root, "data.txt",
          SALTS_FS_O_RDWR | SALTS_FS_O_CREAT, 0600, &file), 0);
      check_equal(cmeta_fs_root_file_write(file, "abc", 3), 3);
      check_equal(cmeta_fs_root_rename(root, "data.txt", root, "inside.txt"), 0);
      cmeta_fs_stat_t stat = {0};
      check_equal(cmeta_fs_root_stat(root, "data.txt", &stat), -ENOENT);
      check_equal(cmeta_fs_root_stat(root, "inside.txt", &stat), 0);
      check_equal(stat.size, 3u);
      check_equal(cmeta_fs_root_file_seek(file, 0, SEEK_SET), 0);
      char text[4] = {0};
      check_equal(cmeta_fs_root_file_read(file, text, 3), 3);
      check_equal(text, "abc");
      check_equal(cmeta_fs_root_file_close(file), 0); file = NULL;
      check_equal(cmeta_fs_root_file_open(root, "data.txt",
          SALTS_FS_O_RDWR | SALTS_FS_O_CREAT, 0600, &file), 0);
      check_equal(cmeta_fs_root_file_write(file, "new", 3), 3);
      check_equal(cmeta_fs_root_rename(root, "data.txt", root, "inside.txt"), 0);
      check_less(cmeta_fs_root_rename(root, "inside.txt", root, "../outside.txt"), 0);
      check_equal(cmeta_fs_root_stat(root, "inside.txt", &stat), 0);
    }
  }
}
