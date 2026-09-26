#ifndef TAI_BOOKMARKS_H
#define TAI_BOOKMARKS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct TaiBookmarks TaiBookmarks;

/* An owned, sorted copy. Each URL and the array are owned by the snapshot. */
typedef struct {
  char **urls;
  size_t count;
} TaiBookmarkSnapshot;

/* One browser owner creates the collection and shares a borrow with tabs and
 * chrome. All operations run on that owner's thread. The create variant is
 * memory-only; persistent variants write synchronously on toggle. */
TaiBookmarks *tai_bookmarks_create(void);
/* Persistent stores use a private per-user file. A NULL result sets an owned
 * diagnostic in error when possible; malformed files are never overwritten. */
TaiBookmarks *tai_bookmarks_open_default(char **error);
/* Explicit path is for embedders and isolated tests. Its parent must exist. */
TaiBookmarks *tai_bookmarks_open_path(const char *path, char **error);
void tai_bookmarks_destroy(TaiBookmarks *bookmarks);

/* URLs are exact serialized committed-page strings. NULL, empty strings,
 * about:blank, and about:bookmarks cannot be bookmarked. */
bool tai_bookmarks_contains(const TaiBookmarks *bookmarks, const char *url);
/* On success, now_bookmarked reports the new state. On invalid input, OOM, or
 * a pre-commit disk failure, returns false, leaves the logical collection and
 * previously committed file unchanged, and does not write output. */
bool tai_bookmarks_toggle(TaiBookmarks *bookmarks, const char *url,
                          bool *now_bookmarked);

/* Replaces an initially empty snapshot with owned copies sorted by URL. On
 * failure, leaves snapshot empty and the collection unchanged. */
bool tai_bookmarks_snapshot(const TaiBookmarks *bookmarks,
                            TaiBookmarkSnapshot *snapshot);
void tai_bookmark_snapshot_destroy(TaiBookmarkSnapshot *snapshot);

#endif
