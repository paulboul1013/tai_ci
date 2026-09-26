#define _POSIX_C_SOURCE 200809L
#include "tai/bookmarks.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linker wrapping fails one allocation in the store without changing its API. */
static long fail_after = -1;
static long fail_fsync_after = -1;
static bool fail_rename_once;
void *__real_malloc(size_t size);
void *__real_realloc(void *ptr, size_t size);
int __real_fsync(int fd);
int __real_rename(const char *old_path, const char *new_path);

static bool should_fail(void) {
  if (fail_after < 0) return false;
  if (fail_after == 0) {
    fail_after = -1;
    return true;
  }
  --fail_after;
  return false;
}

void *__wrap_malloc(size_t size) {
  return should_fail() ? NULL : __real_malloc(size);
}

void *__wrap_realloc(void *ptr, size_t size) {
  return should_fail() ? NULL : __real_realloc(ptr, size);
}

int __wrap_fsync(int fd) {
  if (fail_fsync_after == 0) {
    fail_fsync_after = -1;
    errno = EIO;
    return -1;
  }
  if (fail_fsync_after > 0) --fail_fsync_after;
  return __real_fsync(fd);
}

int __wrap_rename(const char *old_path, const char *new_path) {
  if (fail_rename_once) {
    fail_rename_once = false;
    errno = EIO;
    return -1;
  }
  return __real_rename(old_path, new_path);
}

static void exact_urls_are_shared_sorted_and_independent_of_snapshots(void) {
  TaiBookmarks *bookmarks = tai_bookmarks_create();
  assert(bookmarks);
  TaiBookmarks *other_tab_borrow = bookmarks;
  const char *later = "https://example.test/z?q=two#end";
  const char *earlier = "https://example.test/a?q=one&x=%22";
  bool selected = false;

  assert(tai_bookmarks_toggle(bookmarks, later, &selected) && selected);
  assert(tai_bookmarks_toggle(other_tab_borrow, earlier, &selected) && selected);
  assert(tai_bookmarks_contains(bookmarks, earlier));
  assert(tai_bookmarks_contains(other_tab_borrow, later));
  assert(!tai_bookmarks_contains(bookmarks, "https://example.test/a"));

  TaiBookmarkSnapshot snapshot = {0};
  assert(tai_bookmarks_snapshot(bookmarks, &snapshot));
  assert(snapshot.count == 2);
  assert(strcmp(snapshot.urls[0], earlier) == 0);
  assert(strcmp(snapshot.urls[1], later) == 0);

  assert(tai_bookmarks_toggle(other_tab_borrow, earlier, &selected) && !selected);
  assert(!tai_bookmarks_contains(bookmarks, earlier));
  assert(snapshot.count == 2 && strcmp(snapshot.urls[0], earlier) == 0);
  tai_bookmark_snapshot_destroy(&snapshot);

  assert(tai_bookmarks_snapshot(bookmarks, &snapshot));
  assert(snapshot.count == 1 && strcmp(snapshot.urls[0], later) == 0);
  tai_bookmark_snapshot_destroy(&snapshot);
  tai_bookmarks_destroy(bookmarks);
}

static void empty_and_internal_pages_are_rejected(void) {
  TaiBookmarks *bookmarks = tai_bookmarks_create();
  assert(bookmarks);
  bool selected = true;
  const char *invalid[] = {NULL, "", "about:blank", "about:bookmarks"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    assert(!tai_bookmarks_toggle(bookmarks, invalid[i], &selected));
    assert(selected);
    assert(!tai_bookmarks_contains(bookmarks, invalid[i]));
  }
  TaiBookmarkSnapshot snapshot = {0};
  assert(tai_bookmarks_snapshot(bookmarks, &snapshot));
  assert(snapshot.count == 0 && snapshot.urls == NULL);
  tai_bookmark_snapshot_destroy(&snapshot);
  tai_bookmarks_destroy(bookmarks);
}

static void store_copies_the_callers_url(void) {
  TaiBookmarks *bookmarks = tai_bookmarks_create();
  assert(bookmarks);
  char url[] = "data:text/html,first";
  bool selected = false;
  assert(tai_bookmarks_toggle(bookmarks, url, &selected) && selected);
  url[sizeof(url) - 2] = 'x';
  assert(tai_bookmarks_contains(bookmarks, "data:text/html,first"));
  assert(!tai_bookmarks_contains(bookmarks, url));
  tai_bookmarks_destroy(bookmarks);
}

static void failed_allocations_leave_collection_and_snapshot_intact(void) {
  fail_after = 0;
  assert(!tai_bookmarks_create());

  TaiBookmarks *bookmarks = tai_bookmarks_create();
  assert(bookmarks);
  bool selected = false;
  const char *url = "https://example.test/one";
  for (long step = 0; step < 2; ++step) {
    fail_after = step;
    assert(!tai_bookmarks_toggle(bookmarks, url, &selected));
    assert(!selected);
    assert(!tai_bookmarks_contains(bookmarks, url));
  }
  assert(tai_bookmarks_toggle(bookmarks, url, &selected) && selected);
  assert(tai_bookmarks_toggle(bookmarks, "https://example.test/two", &selected));

  for (long step = 0; step < 3; ++step) {
    TaiBookmarkSnapshot snapshot = {0};
    fail_after = step;
    assert(!tai_bookmarks_snapshot(bookmarks, &snapshot));
    assert(snapshot.urls == NULL && snapshot.count == 0);
    assert(tai_bookmarks_contains(bookmarks, url));
    tai_bookmark_snapshot_destroy(&snapshot);
  }
  TaiBookmarkSnapshot snapshot = {0};
  assert(tai_bookmarks_snapshot(bookmarks, &snapshot));
  assert(snapshot.count == 2);
  tai_bookmark_snapshot_destroy(&snapshot);
  tai_bookmarks_destroy(bookmarks);
}

static void persistent_collection_survives_restart_and_removal(void) {
  char dir[] = "/tmp/tai-bookmarks-XXXXXX";
  assert(mkdtemp(dir));
  char path[sizeof(dir) + 16];
  assert(snprintf(path, sizeof(path), "%s/bookmarks", dir) > 0);
  char *error = NULL;
  TaiBookmarks *first = tai_bookmarks_open_path(path, &error);
  assert(first && !error);
  bool selected = false;
  assert(tai_bookmarks_toggle(first, "https://example.test/z", &selected));
  assert(tai_bookmarks_toggle(first, "https://example.test/a", &selected));
  tai_bookmarks_destroy(first);

  TaiBookmarks *second = tai_bookmarks_open_path(path, &error);
  assert(second && !error);
  TaiBookmarkSnapshot snapshot = {0};
  assert(tai_bookmarks_snapshot(second, &snapshot));
  assert(snapshot.count == 2);
  assert(strcmp(snapshot.urls[0], "https://example.test/a") == 0);
  assert(strcmp(snapshot.urls[1], "https://example.test/z") == 0);
  tai_bookmark_snapshot_destroy(&snapshot);
  assert(tai_bookmarks_toggle(second, "https://example.test/a", &selected));
  assert(!selected);
  tai_bookmarks_destroy(second);

  TaiBookmarks *third = tai_bookmarks_open_path(path, &error);
  assert(third && !error);
  assert(!tai_bookmarks_contains(third, "https://example.test/a"));
  assert(tai_bookmarks_contains(third, "https://example.test/z"));
  tai_bookmarks_destroy(third);
  assert(unlink(path) == 0);
  assert(rmdir(dir) == 0);
}

static void malformed_file_is_rejected_without_replacement(void) {
  char dir[] = "/tmp/tai-bookmarks-bad-XXXXXX";
  assert(mkdtemp(dir));
  char path[sizeof(dir) + 16];
  assert(snprintf(path, sizeof(path), "%s/bookmarks", dir) > 0);
  FILE *file = fopen(path, "wb");
  assert(file && fputs("invalid bookmark file", file) >= 0 && fclose(file) == 0);
  char *error = NULL;
  assert(!tai_bookmarks_open_path(path, &error));
  assert(error);
  free(error);
  file = fopen(path, "rb");
  assert(file && fgetc(file) == 'i' && fclose(file) == 0);
  assert(unlink(path) == 0);
  assert(rmdir(dir) == 0);
}

static void precommit_disk_failures_keep_memory_and_disk_in_sync(void) {
  char dir[] = "/tmp/tai-bookmarks-fail-XXXXXX";
  assert(mkdtemp(dir));
  char path[sizeof(dir) + 16];
  assert(snprintf(path, sizeof(path), "%s/bookmarks", dir) > 0);
  char *error = NULL;
  TaiBookmarks *bookmarks = tai_bookmarks_open_path(path, &error);
  assert(bookmarks && !error);
  bool selected = false;
  assert(tai_bookmarks_toggle(bookmarks, "https://example.test/kept", &selected));

  fail_fsync_after = 0;
  assert(!tai_bookmarks_toggle(bookmarks, "https://example.test/lost", &selected));
  assert(!tai_bookmarks_contains(bookmarks, "https://example.test/lost"));
  fail_rename_once = true;
  assert(!tai_bookmarks_toggle(bookmarks, "https://example.test/lost", &selected));
  assert(!tai_bookmarks_contains(bookmarks, "https://example.test/lost"));

  /* Directory sync is after rename: the newly visible file and memory must
   * commit together even if that final durability attempt fails. */
  fail_fsync_after = 1;
  assert(tai_bookmarks_toggle(bookmarks, "https://example.test/committed", &selected));
  assert(tai_bookmarks_contains(bookmarks, "https://example.test/committed"));

  tai_bookmarks_destroy(bookmarks);
  bookmarks = tai_bookmarks_open_path(path, &error);
  assert(bookmarks && !error);
  assert(tai_bookmarks_contains(bookmarks, "https://example.test/kept"));
  assert(tai_bookmarks_contains(bookmarks, "https://example.test/committed"));
  assert(!tai_bookmarks_contains(bookmarks, "https://example.test/lost"));
  tai_bookmarks_destroy(bookmarks);
  assert(unlink(path) == 0);
  assert(rmdir(dir) == 0);
}

static void default_store_uses_private_xdg_directory(void) {
  char dir[] = "/tmp/tai-bookmarks-xdg-XXXXXX";
  assert(mkdtemp(dir));
  const char *previous = getenv("XDG_DATA_HOME");
  char *saved = previous ? strdup(previous) : NULL;
  assert(!previous || saved);
  assert(setenv("XDG_DATA_HOME", dir, 1) == 0);

  char *error = NULL;
  TaiBookmarks *bookmarks = tai_bookmarks_open_default(&error);
  assert(bookmarks && !error);
  bool selected = false;
  assert(tai_bookmarks_toggle(bookmarks, "https://example.test/xdg", &selected));
  tai_bookmarks_destroy(bookmarks);

  char folder[sizeof(dir) + 16], path[sizeof(dir) + 32];
  assert(snprintf(folder, sizeof(folder), "%s/tai-browser", dir) > 0);
  assert(snprintf(path, sizeof(path), "%s/bookmarks", folder) > 0);
  struct stat info;
  assert(stat(folder, &info) == 0 && S_ISDIR(info.st_mode));
  assert((info.st_mode & 077) == 0);
  assert(stat(path, &info) == 0 && S_ISREG(info.st_mode));
  assert((info.st_mode & 077) == 0);
  bookmarks = tai_bookmarks_open_default(&error);
  assert(bookmarks && !error);
  assert(tai_bookmarks_contains(bookmarks, "https://example.test/xdg"));
  tai_bookmarks_destroy(bookmarks);

  if (saved) assert(setenv("XDG_DATA_HOME", saved, 1) == 0);
  else assert(unsetenv("XDG_DATA_HOME") == 0);
  free(saved);
  assert(unlink(path) == 0);
  assert(rmdir(folder) == 0);
  assert(rmdir(dir) == 0);
}

int main(void) {
  exact_urls_are_shared_sorted_and_independent_of_snapshots();
  empty_and_internal_pages_are_rejected();
  store_copies_the_callers_url();
  failed_allocations_leave_collection_and_snapshot_intact();
  persistent_collection_survives_restart_and_removal();
  malformed_file_is_rejected_without_replacement();
  precommit_disk_failures_keep_memory_and_disk_in_sync();
  default_store_uses_private_xdg_directory();
  return 0;
}
