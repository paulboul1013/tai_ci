#include "presentation_internal.h"

static bool present_tabs_scene(SDL_Renderer *renderer,
                               SDL_Texture *page_texture,
                               SDL_Texture *chrome_texture,
                               const TaiTabSetView *view, int width,
                               int height) {
  if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
      !SDL_RenderClear(renderer)) return false;
  if (view->page && page_texture) {
    SDL_FRect page_rect = {0.0f, (float)tabs_chrome_bottom(width),
                           (float)width,
                           (float)tabs_content_pixel_height(width, height)};
    if (!SDL_RenderTexture(renderer, page_texture, NULL, &page_rect))
      return false;
    TaiScrollbarRect bar;
    if (tai_scrollbar_geometry(width, tabs_content_height(width, height),
                               tai_page_scroll_y(view->page),
                               tai_page_max_scroll_y(view->page), &bar)) {
      SDL_FRect rect = {bar.x, (float)(bar.y + tabs_chrome_bottom(width)),
                        bar.w, bar.h};
      if (!SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255) ||
          !SDL_RenderFillRect(renderer, &rect)) return false;
    }
  }
  SDL_FRect chrome_rect = {0.0f, 0.0f, (float)width,
                           (float)ceil(tabs_chrome_bottom(width))};
  if (!SDL_RenderTexture(renderer, chrome_texture, NULL, &chrome_rect))
    return false;
  return SDL_RenderPresent(renderer);
}

bool tai_pres_present_scene(SDL_Renderer *renderer, SDL_Texture *page_texture,
                            SDL_Texture *chrome_texture, const TaiPage *page,
                            int width, int height, bool chrome_enabled) {
  if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
      !SDL_RenderClear(renderer)) return false;
  double view_height = content_height(width, height, chrome_enabled);
  if (chrome_enabled) {
    SDL_FRect page_rect = {0.0f, (float)chrome_bottom(width), (float)width,
                           (float)content_pixel_height(width, height, true)};
    if (!SDL_RenderTexture(renderer, page_texture, NULL, &page_rect))
      return false;
  } else if (!SDL_RenderTexture(renderer, page_texture, NULL, NULL)) {
    return false;
  }
  TaiScrollbarRect bar;
  if (tai_scrollbar_geometry(width, view_height, tai_page_scroll_y(page),
                             tai_page_max_scroll_y(page), &bar)) {
    SDL_FRect rect = {bar.x,
                      (float)(bar.y + (chrome_enabled ? chrome_bottom(width) : 0)),
                      bar.w, bar.h};
    if (!SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255) ||
        !SDL_RenderFillRect(renderer, &rect)) return false;
  }
  if (chrome_enabled) {
    SDL_FRect chrome_rect = {0.0f, 0.0f, (float)width,
                             (float)ceil(chrome_bottom(width))};
    if (!SDL_RenderTexture(renderer, chrome_texture, NULL, &chrome_rect))
      return false;
  }
  return SDL_RenderPresent(renderer);
}

static bool update_page_texture(SDL_Renderer *renderer,
                                SDL_Texture **texture, const TaiPage *page,
                                int width, int window_height,
                                bool chrome_enabled, char **error) {
  if (!page) {
    SDL_DestroyTexture(*texture);
    *texture = NULL;
    return true;
  }
  int height = content_pixel_height(width, window_height, chrome_enabled);
  if (!valid_pixel_dimensions(width, window_height) || height <= 0 ||
      !valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported window pixel dimensions");
    return false;
  }
  unsigned char *pixels = NULL;
  int stride = 0;
  const TaiDisplayList *list = tai_page_display_list(page);
  if (!list || !tai_display_list_raster_region(list, width, height, 0,
                                      tai_page_scroll_y(page),
                                      &pixels, &stride, error)) return false;
  SDL_Texture *next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, width, height);
  /* Cairo ARGB32 and SDL ARGB8888 are both native-endian 0xAARRGGBB.
   * Opaque white backing means there is no premultiplied-alpha blend seam. */
  bool ok = next && SDL_UpdateTexture(next, NULL, pixels, stride);
  free(pixels);
  if (!ok) {
    set_error(error, SDL_GetError());
    SDL_DestroyTexture(next);
    return false;
  }
  SDL_DestroyTexture(*texture);
  *texture = next;
  return true;
}

static bool update_tabs_page_texture(SDL_Renderer *renderer,
                                     SDL_Texture **texture,
                                     const TaiPage *page, int width,
                                     int window_height, char **error) {
  if (!page) {
    SDL_DestroyTexture(*texture);
    *texture = NULL;
    return true;
  }
  int height = tabs_content_pixel_height(width, window_height);
  if (!valid_pixel_dimensions(width, window_height) || height <= 0 ||
      !valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported tab window pixel dimensions");
    return false;
  }
  unsigned char *pixels = NULL;
  int stride = 0;
  const TaiDisplayList *list = tai_page_display_list(page);
  if (!list || !tai_display_list_raster_region(list, width, height, 0,
                                      tai_page_scroll_y(page),
                                      &pixels, &stride, error)) return false;
  SDL_Texture *next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, width,
                                       height);
  bool ok = next && SDL_UpdateTexture(next, NULL, pixels, stride);
  free(pixels);
  if (!ok) {
    set_error(error, SDL_GetError());
    SDL_DestroyTexture(next);
    return false;
  }
  SDL_DestroyTexture(*texture);
  *texture = next;
  return true;
}

bool tai_pres_repaint_scene(SDL_Renderer *renderer, SDL_Texture **page_texture,
                            SDL_Texture **chrome_texture, const TaiPage *page,
                            int width, int height, bool chrome_enabled,
                            bool update_page, bool update_chrome,
                            const AddressEditor *editor,
                            const TaiPresentWindowCallbacks *callbacks,
                            char **error) {
  if (update_page && !update_page_texture(renderer, page_texture, page,
                                           width, height, chrome_enabled,
                                           error)) return false;
  if (chrome_enabled && update_chrome &&
      !tai_pres_render_chrome_texture(renderer, chrome_texture, page, width,
                                      editor, callbacks, error)) return false;
  if (!tai_pres_present_scene(renderer, *page_texture, *chrome_texture, page,
                              width, height, chrome_enabled)) {
    set_error(error, SDL_GetError());
    return false;
  }
  return true;
}

bool tai_pres_repaint_tabs_scene(SDL_Renderer *renderer,
                              SDL_Texture **page_texture,
                              SDL_Texture **chrome_texture,
                              const TaiTabSetView *view, int width,
                              int height, bool update_page,
                              bool update_chrome,
                              const AddressEditor *editor, char **error) {
  if (update_page && !update_tabs_page_texture(renderer, page_texture,
          view->page, width, height, error)) return false;
  if (update_chrome && !tai_pres_render_tabs_chrome_texture(renderer,
          chrome_texture, view, width, editor, error)) return false;
  if (!present_tabs_scene(renderer, *page_texture, *chrome_texture, view,
                          width, height)) {
    set_error(error, SDL_GetError());
    return false;
  }
  return true;
}
