#define _GNU_SOURCE
#include "tai/bookmarks.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { MAX_FILE_BYTES = 16 * 1024 * 1024 };
static const unsigned char file_magic[8] = {'T', 'A', 'I', 'B', 'K', 'M', '1', '\n'};

struct TaiBookmarks {
  char **urls;
  size_t count;
  size_t capacity;
  char *path;
};

static void set_error(char **error, const char *message) {
  if (!error || *error) return;
  size_t length = strlen(message);
  *error = malloc(length + 1);
  if (*error) memcpy(*error, message, length + 1);
}

static char *copy_string(const char *text) {
  size_t length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  char *copy = malloc(length + 1);
  if (copy) memcpy(copy, text, length + 1);
  return copy;
}

static bool read_all(int fd, unsigned char *buffer, size_t length) {
  while (length) {
    ssize_t got = read(fd, buffer, length);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    buffer += (size_t)got;
    length -= (size_t)got;
  }
  return true;
}

static bool write_all(int fd, const unsigned char *buffer, size_t length) {
  while (length) {
    ssize_t done = write(fd, buffer, length);
    if (done < 0 && errno == EINTR) continue;
    if (done <= 0) return false;
    buffer += (size_t)done;
    length -= (size_t)done;
  }
  return true;
}

static uint32_t read_u32(const unsigned char *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool write_u32(int fd, uint32_t number) {
  unsigned char bytes[4] = {(unsigned char)number,
                            (unsigned char)(number >> 8),
                            (unsigned char)(number >> 16),
                            (unsigned char)(number >> 24)};
  return write_all(fd, bytes, sizeof(bytes));
}

static char *parent_directory(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return copy_string(".");
  if (slash == path) return copy_string("/");
  size_t length = (size_t)(slash - path);
  char *parent = malloc(length + 1);
  if (!parent) return NULL;
  memcpy(parent, path, length);
  parent[length] = '\0';
  return parent;
}

static bool persist_candidate(const TaiBookmarks *bookmarks, const char *url,
                              size_t index, bool adding) {
  if (!bookmarks->path) return true;
  size_t next_count = bookmarks->count + (adding ? 1 : 0) - (adding ? 0 : 1);
  if (next_count > UINT32_MAX) return false;
  size_t total = sizeof(file_magic) + 4;
  for (size_t i = 0; i < next_count; ++i) {
    const char *item = adding ? (i < index ? bookmarks->urls[i]
                                   : i == index ? url : bookmarks->urls[i - 1])
                              : bookmarks->urls[i < index ? i : i + 1];
    size_t length = strlen(item);
    if (length > UINT32_MAX || total > MAX_FILE_BYTES - 4 ||
        length > MAX_FILE_BYTES - total - 4) return false;
    total += 4 + length;
  }

  char *parent = parent_directory(bookmarks->path);
  if (!parent) return false;
  int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  free(parent);
  if (directory < 0) return false;
  size_t path_length = strlen(bookmarks->path);
  static const char suffix[] = ".tmp.XXXXXX";
  if (path_length > SIZE_MAX - sizeof(suffix)) {
    close(directory);
    return false;
  }
  char *temporary = malloc(path_length + sizeof(suffix));
  if (!temporary) {
    close(directory);
    return false;
  }
  memcpy(temporary, bookmarks->path, path_length);
  memcpy(temporary + path_length, suffix, sizeof(suffix));
  int fd = mkstemp(temporary);
  if (fd < 0) {
    free(temporary);
    close(directory);
    return false;
  }

  bool ok = write_all(fd, file_magic, sizeof(file_magic)) &&
            write_u32(fd, (uint32_t)next_count);
  for (size_t i = 0; ok && i < next_count; ++i) {
    const char *item = adding ? (i < index ? bookmarks->urls[i]
                                   : i == index ? url : bookmarks->urls[i - 1])
                              : bookmarks->urls[i < index ? i : i + 1];
    size_t length = strlen(item);
    ok = write_u32(fd, (uint32_t)length) &&
         write_all(fd, (const unsigned char *)item, length);
  }
  if (ok) ok = fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (ok) ok = rename(temporary, bookmarks->path) == 0;
  if (!ok) unlink(temporary);
  else (void)fsync(directory); /* Rename is the commit point. */
  free(temporary);
  close(directory);
  return ok;
}

static bool valid_url(const char *url) {
  return url && *url && strcmp(url, "about:blank") != 0 &&
         strcmp(url, "about:bookmarks") != 0;
}

static bool load_file(TaiBookmarks *bookmarks, const char *path, char **error) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) return true;
    set_error(error, "Cannot open bookmarks file");
    return false;
  }
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size < (off_t)(sizeof(file_magic) + 4) ||
      info.st_size > MAX_FILE_BYTES) {
    close(fd);
    set_error(error, "Invalid bookmarks file");
    return false;
  }
  size_t file_size = (size_t)info.st_size;
  unsigned char *bytes = malloc(file_size);
  if (!bytes) {
    close(fd);
    set_error(error, "Out of memory loading bookmarks");
    return false;
  }
  bool read_ok = read_all(fd, bytes, file_size);
  unsigned char extra;
  if (read_ok) {
    ssize_t got = read(fd, &extra, 1);
    if (got != 0) read_ok = false;
  }
  if (close(fd) != 0) read_ok = false;
  if (!read_ok || memcmp(bytes, file_magic, sizeof(file_magic)) != 0) {
    free(bytes);
    set_error(error, "Invalid bookmarks file");
    return false;
  }
  size_t position = sizeof(file_magic);
  uint32_t count = read_u32(bytes + position);
  position += 4;
  if (count > (file_size - position) / 5) {
    free(bytes);
    set_error(error, "Invalid bookmarks file");
    return false;
  }
  if (count) {
    bookmarks->urls = malloc((size_t)count * sizeof(*bookmarks->urls));
    if (!bookmarks->urls) {
      free(bytes);
      set_error(error, "Out of memory loading bookmarks");
      return false;
    }
    bookmarks->capacity = count;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (file_size - position < 4) break;
    uint32_t length = read_u32(bytes + position);
    position += 4;
    if (length == 0 || length > file_size - position ||
        memchr(bytes + position, '\0', length)) break;
    char *url = malloc((size_t)length + 1);
    if (!url) {
      free(bytes);
      set_error(error, "Out of memory loading bookmarks");
      return false;
    }
    memcpy(url, bytes + position, length);
    url[length] = '\0';
    if (!valid_url(url) ||
        (bookmarks->count && strcmp(bookmarks->urls[bookmarks->count - 1], url) >= 0)) {
      free(url);
      break;
    }
    bookmarks->urls[bookmarks->count++] = url;
    position += length;
  }
  free(bytes);
  if (bookmarks->count != count || position != file_size) {
    set_error(error, "Invalid bookmarks file");
    return false;
  }
  return true;
}

TaiBookmarks *tai_bookmarks_open_path(const char *path, char **error) {
  if (error) *error = NULL;
  if (!path || !*path || path[strlen(path) - 1] == '/') {
    set_error(error, "Invalid bookmarks path");
    return NULL;
  }
  TaiBookmarks *bookmarks = tai_bookmarks_create();
  if (!bookmarks) {
    set_error(error, "Out of memory opening bookmarks");
    return NULL;
  }
  if (!load_file(bookmarks, path, error)) {
    tai_bookmarks_destroy(bookmarks);
    return NULL;
  }
  bookmarks->path = copy_string(path);
  if (!bookmarks->path) {
    tai_bookmarks_destroy(bookmarks);
    set_error(error, "Out of memory opening bookmarks");
    return NULL;
  }
  return bookmarks;
}

static bool make_directories(const char *path) {
  char *copy = copy_string(path);
  if (!copy) return false;
  bool ok = true;
  for (char *part = copy + 1; ; ++part) {
    if (*part != '/' && *part != '\0') continue;
    char saved = *part;
    *part = '\0';
    if (mkdir(copy, 0700) != 0 && errno != EEXIST) ok = false;
    if (ok) {
      struct stat info;
      if (stat(copy, &info) != 0 || !S_ISDIR(info.st_mode)) ok = false;
    }
    *part = saved;
    if (!ok || saved == '\0') break;
  }
  free(copy);
  return ok;
}

TaiBookmarks *tai_bookmarks_open_default(char **error) {
  if (error) *error = NULL;
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  const char *base = xdg && *xdg == '/' ? xdg : home;
  const char *suffix = xdg && *xdg == '/' ? "/tai-browser/bookmarks"
                                          : "/.local/share/tai-browser/bookmarks";
  if (!base || *base != '/') {
    set_error(error, "Cannot locate user data directory");
    return NULL;
  }
  size_t base_length = strlen(base), suffix_length = strlen(suffix);
  if (base_length > SIZE_MAX - suffix_length - 1) {
    set_error(error, "Invalid user data directory");
    return NULL;
  }
  char *path = malloc(base_length + suffix_length + 1);
  if (!path) {
    set_error(error, "Out of memory opening bookmarks");
    return NULL;
  }
  memcpy(path, base, base_length);
  memcpy(path + base_length, suffix, suffix_length + 1);
  char *parent = parent_directory(path);
  if (!parent || !make_directories(parent)) {
    free(parent);
    free(path);
    set_error(error, "Cannot create bookmarks directory");
    return NULL;
  }
  free(parent);
  TaiBookmarks *bookmarks = tai_bookmarks_open_path(path, error);
  free(path);
  return bookmarks;
}

/* The collection stays sorted, so snapshots need no sorting or mutation. */
static size_t lower_bound(const TaiBookmarks *bookmarks, const char *url) {
  size_t first = 0;
  size_t count = bookmarks->count;
  while (count) {
    size_t step = count / 2;
    size_t middle = first + step;
    if (strcmp(bookmarks->urls[middle], url) < 0) {
      first = middle + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  return first;
}

TaiBookmarks *tai_bookmarks_create(void) {
  TaiBookmarks *bookmarks = malloc(sizeof(*bookmarks));
  if (!bookmarks) return NULL;
  *bookmarks = (TaiBookmarks){0};
  return bookmarks;
}

void tai_bookmarks_destroy(TaiBookmarks *bookmarks) {
  if (!bookmarks) return;
  for (size_t i = 0; i < bookmarks->count; ++i) free(bookmarks->urls[i]);
  free(bookmarks->urls);
  free(bookmarks->path);
  free(bookmarks);
}

bool tai_bookmarks_contains(const TaiBookmarks *bookmarks, const char *url) {
  if (!bookmarks || !valid_url(url)) return false;
  size_t index = lower_bound(bookmarks, url);
  return index < bookmarks->count && strcmp(bookmarks->urls[index], url) == 0;
}

bool tai_bookmarks_toggle(TaiBookmarks *bookmarks, const char *url,
                          bool *now_bookmarked) {
  if (!bookmarks || !valid_url(url) || !now_bookmarked) return false;
  size_t index = lower_bound(bookmarks, url);
  if (index < bookmarks->count && strcmp(bookmarks->urls[index], url) == 0) {
    if (!persist_candidate(bookmarks, url, index, false)) return false;
    free(bookmarks->urls[index]);
    memmove(bookmarks->urls + index, bookmarks->urls + index + 1,
            (bookmarks->count - index - 1) * sizeof(*bookmarks->urls));
    --bookmarks->count;
    *now_bookmarked = false;
    return true;
  }

  size_t length = strlen(url);
  if (length == SIZE_MAX) return false;
  char *copy = malloc(length + 1);
  if (!copy) return false;
  memcpy(copy, url, length + 1);

  if (bookmarks->count == bookmarks->capacity) {
    if (bookmarks->count == SIZE_MAX / sizeof(*bookmarks->urls)) {
      free(copy);
      return false;
    }
    size_t maximum = SIZE_MAX / sizeof(*bookmarks->urls);
    size_t next = bookmarks->capacity < maximum / 2
                      ? bookmarks->capacity * 2
                      : maximum;
    if (next < 4) next = 4;
    char **grown = realloc(bookmarks->urls, next * sizeof(*grown));
    if (!grown) {
      free(copy);
      return false;
    }
    bookmarks->urls = grown;
    bookmarks->capacity = next;
  }
  if (!persist_candidate(bookmarks, url, index, true)) {
    free(copy);
    return false;
  }
  memmove(bookmarks->urls + index + 1, bookmarks->urls + index,
          (bookmarks->count - index) * sizeof(*bookmarks->urls));
  bookmarks->urls[index] = copy;
  ++bookmarks->count;
  *now_bookmarked = true;
  return true;
}

bool tai_bookmarks_snapshot(const TaiBookmarks *bookmarks,
                            TaiBookmarkSnapshot *snapshot) {
  if (!bookmarks || !snapshot || snapshot->urls || snapshot->count) return false;
  if (!bookmarks->count) return true;
  if (bookmarks->count > SIZE_MAX / sizeof(*snapshot->urls)) return false;

  char **copies = malloc(bookmarks->count * sizeof(*copies));
  if (!copies) return false;
  size_t copied = 0;
  for (; copied < bookmarks->count; ++copied) {
    size_t length = strlen(bookmarks->urls[copied]);
    copies[copied] = malloc(length + 1);
    if (!copies[copied]) break;
    memcpy(copies[copied], bookmarks->urls[copied], length + 1);
  }
  if (copied != bookmarks->count) {
    for (size_t i = 0; i < copied; ++i) free(copies[i]);
    free(copies);
    return false;
  }
  snapshot->urls = copies;
  snapshot->count = copied;
  return true;
}

void tai_bookmark_snapshot_destroy(TaiBookmarkSnapshot *snapshot) {
  if (!snapshot) return;
  for (size_t i = 0; i < snapshot->count; ++i) free(snapshot->urls[i]);
  free(snapshot->urls);
  *snapshot = (TaiBookmarkSnapshot){0};
}
