#ifndef TAI_SESSION_H
#define TAI_SESSION_H

#include "tai/browser.h"

typedef struct TaiSession TaiSession;
/* Takes initial_page on success. Network and CSS remain borrowed. */
TaiSession *tai_session_create(TaiNetwork *network, TaiPage *initial_page,
                               const char *default_css, bool rtl);
/* Creates an empty tab session for asynchronous page loading. It owns no
 * network handle; candidates are transferred by the commit functions below. */
TaiSession *tai_session_create_empty(const char *default_css, bool rtl);
void tai_session_destroy(TaiSession *session);
TaiPage *tai_session_page(const TaiSession *session);
size_t tai_session_history_length(const TaiSession *session);
size_t tai_session_history_index(const TaiSession *session);
/* Returns an owned history URL for an in-range index. */
char *tai_session_history_url(const TaiSession *session, size_t index);
/* All operations leave page and history unchanged if loading or allocation
 * fails. History stores only owned URL strings; traversal always uses GET. */
bool tai_session_navigate(TaiSession *session,
                          const TaiNavigationIntent *intent, char **error);
/* Returns an owned serialized URL following the Python Chrome address bar
 * rules, or NULL for malformed direct URLs/allocation failure. */
char *tai_session_normalize_address(const char *text);
/* Loads an address bar submission as GET and commits page/history together. */
bool tai_session_navigate_address(TaiSession *session, const char *text,
                                  char **error);
bool tai_session_record_fragment(TaiSession *session, const char *url,
                                  char **error);
/* Returns an owned URL and its target index for an available history move. */
bool tai_session_history_target(const TaiSession *session, int direction,
                                char **url, size_t *target_index);
/* On success, transfers candidate ownership into the session. Failed
 * navigation commits append history; successful history traversal replaces
 * the page and index without adding an entry. */
bool tai_session_commit_navigation(TaiSession *session, TaiPage *candidate,
                                   char **error);
bool tai_session_commit_history(TaiSession *session, TaiPage *candidate,
                                size_t target_index, char **error);
bool tai_session_back(TaiSession *session, char **error);
bool tai_session_forward(TaiSession *session, char **error);

#endif
