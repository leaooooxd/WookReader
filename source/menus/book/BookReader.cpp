#include "BookReader.hpp"

#include <libconfig.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "CBZPageLayout.hpp"
#include "LandscapePageLayout.hpp"
#include "PageLayout.hpp"
#include "VerticalPageLayout.hpp"

extern void Log_Write(const std::string& msg);
extern void Log_Error(const std::string& msg);

extern "C" {
#include "SDL_helper.h"
#include "common.h"
#include "config.h"
#include "fs.h"
#include "status_bar.h"
#include "textures.h"
}

int windowX, windowY;
config_t* config = NULL;
const char* configFile = "/switch/WookReader/saved_pages.cfg";

static const char* NOTES_DIR = "/switch/WookReader/.notes";

// Reader UI palette. These elements deliberately stay independent from the
// chooser's system-status header: an open page remains distraction-free.
static const SDL_Color UI_SURFACE_ELEVATED = {30, 30, 34, 250};
static const SDL_Color UI_BORDER = {55, 55, 62, 255};
static const SDL_Color UI_TEXT = {235, 235, 238, 255};
static const SDL_Color UI_TEXT_MUTED = {150, 150, 160, 255};
static const SDL_Color UI_ACCENT = {192, 84, 78, 255};
static const SDL_Color UI_ACCENT_SOFT = {192, 84, 78, 54};

static void draw_rounded_rect(SDL_Renderer* renderer, SDL_Rect rectangle,
                              int radius, SDL_Color color) {
  if (rectangle.w <= 0 || rectangle.h <= 0)
    return;

  radius = std::min(radius,
                    std::min(rectangle.w / 2, rectangle.h / 2));

  SDL_BlendMode previous_blend;
  SDL_GetRenderDrawBlendMode(renderer, &previous_blend);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

  for (int y = 0; y < rectangle.h; y++) {
    int inset = 0;
    if (radius > 0 && (y < radius || y >= rectangle.h - radius)) {
      const float distance =
          y < radius
              ? radius - y - 0.5f
              : y - (rectangle.h - radius) + 0.5f;
      inset = radius -
              (int)std::sqrt(radius * radius - distance * distance);
    }

    SDL_RenderDrawLine(renderer,
                       rectangle.x + inset,
                       rectangle.y + y,
                       rectangle.x + rectangle.w - inset - 1,
                       rectangle.y + y);
  }

  SDL_SetRenderDrawBlendMode(renderer, previous_blend);
}

static void draw_reader_backdrop(unsigned char opacity) {
  SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(RENDERER, &previous_blend);
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(RENDERER, 0, 0, 0, opacity);

  SDL_Rect backdrop = {0, 0, windowX, windowY};
  SDL_RenderFillRect(RENDERER, &backdrop);

  SDL_SetRenderDrawBlendMode(RENDERER, previous_blend);
}

static void draw_centered_reader_message(const char* title,
                                         const char* detail = nullptr) {
  const int panel_width = 520;
  const int panel_height = detail ? 142 : 104;
  const int panel_x = (windowX - panel_width) / 2;
  const int panel_y = (windowY - panel_height) / 2;
  SDL_Rect panel = {panel_x, panel_y, panel_width, panel_height};
  draw_rounded_rect(RENDERER, panel, 18, UI_SURFACE_ELEVATED);

  int title_width = 0;
  TTF_SizeUTF8(ROBOTO_25, title, &title_width, nullptr);
  SDL_DrawText(RENDERER, ROBOTO_25,
               panel_x + (panel_width - title_width) / 2,
               panel_y + 31, UI_TEXT, title);

  if (detail) {
    int detail_width = 0;
    TTF_SizeUTF8(ROBOTO_15, detail, &detail_width, nullptr);
    SDL_DrawText(RENDERER, ROBOTO_15,
                 panel_x + (panel_width - detail_width) / 2,
                 panel_y + 83, UI_TEXT_MUTED, detail);
  }
}

static std::string notes_path(const char* book_name) {
  return std::string(NOTES_DIR) + "/" + book_name + ".txt";
}

static std::string notes_load(const char* book_name) {
  std::string path = notes_path(book_name);
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return "";
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return ""; }
  std::string text((size_t)sz, '\0');
  size_t got = fread(&text[0], 1, (size_t)sz, f);
  fclose(f);
  text.resize(got);
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

static void notes_save(const char* book_name, const std::string& text) {
  FS_RecursiveMakeDir(NOTES_DIR);
  std::string path = notes_path(book_name);
  if (text.empty()) {
    remove(path.c_str());
    return;
  }
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
}

static int load_last_page(const char* book_name) {
  if (!config) {
    config = (config_t*)malloc(sizeof(config_t));
    config_init(config);
    config_read_file(config, configFile);
  }

  config_setting_t* setting =
      config_setting_get_member(config_root_setting(config), book_name);

  if (setting) {
    return config_setting_get_int(setting);
  }

  return 0;
}

static void save_last_page(const char* book_name, int current_page) {
  config_setting_t* setting =
      config_setting_get_member(config_root_setting(config), book_name);

  if (!setting) {
    setting = config_setting_add(config_root_setting(config), book_name,
                                 CONFIG_TYPE_INT);
  }

  if (setting) {
    config_setting_set_int(setting, current_page);
    config_write_file(config, configFile);
  }
}

// Precondition: config is already initialized (always called after load_last_page).
static void save_total_pages(const char* book_name, int total) {
  std::string key = std::string(book_name) + "_T";
  config_setting_t* setting =
      config_setting_get_member(config_root_setting(config), key.c_str());
  if (!setting)
    setting = config_setting_add(config_root_setting(config), key.c_str(),
                                 CONFIG_TYPE_INT);
  if (setting) {
    config_setting_set_int(setting, total);
    config_write_file(config, configFile);
  }
}
static void register_manual_page(const char* book_name,
                                 int previous_page,
                                 int current_page,
                                 int total_pages) {
  if (!config || total_pages <= 0)
    return;

  if (current_page != previous_page + 1)
    return;

  std::string progress_key = std::string(book_name) + "_R";

  config_setting_t* progress =
      config_setting_get_member(config_root_setting(config),
                                progress_key.c_str());

  int furthest_page = progress ? config_setting_get_int(progress) : 0;

  if (previous_page > furthest_page)
    return;

  if (current_page <= furthest_page)
    return;

  if (!progress) {
    progress = config_setting_add(config_root_setting(config),
                                  progress_key.c_str(),
                                  CONFIG_TYPE_INT);
  }

  if (!progress)
    return;

  config_setting_set_int(progress, current_page);

  if (current_page >= total_pages - 1) {
    std::string completed_key = std::string(book_name) + "_C";

    config_setting_t* completed =
        config_setting_get_member(config_root_setting(config),
                                  completed_key.c_str());

    if (!completed) {
      completed = config_setting_add(config_root_setting(config),
                                     completed_key.c_str(),
                                     CONFIG_TYPE_BOOL);
    }

    if (completed)
      config_setting_set_bool(completed, true);
  }
}

// Returns true if the path is a comic archive (CBZ/CBR/CBT/CB7,
// case-insensitive)
static bool path_is_cbz(const char* path) {
  const char* dot = strrchr(path, '.');
  if (!dot) return false;
  char ext[8] = {};
  for (int i = 0; i < 7 && dot[i]; i++)
    ext[i] = (char)tolower((unsigned char)dot[i]);
  return strcmp(ext, ".cbz") == 0 || strcmp(ext, ".cbr") == 0 ||
         strcmp(ext, ".cbt") == 0 || strcmp(ext, ".cb7") == 0;
}

BookReader::BookReader(const char* path, int* result) {
  SDL_GetWindowSize(WINDOW, &windowX, &windowY);

  book_path = path;
  book_name =
      std::string(path).substr(std::string(path).find_last_of("/\\") + 1);

  std::string invalid_chars = " :/?#[]@!$&'()*+,;=.";
  for (char& c : invalid_chars)
    book_name.erase(std::remove(book_name.begin(), book_name.end(), c),
                    book_name.end());

  _notes = notes_load(book_name.c_str());

  _is_cbz = path_is_cbz(path);

  if (_is_cbz) {
    Log_Write(std::string("BookReader: opening as comic ZIP: ") + path);
    int current_page = load_last_page(book_name.c_str());
    switch_current_page_layout(_currentPageLayout, current_page);
    if (!layout) {
      Log_Error(std::string("BookReader: CBZ/CBR layout creation failed: ") + path);
      *result = -1;
      return;
    }
    // Layout was created — enumeration may still be running in background.
    // Validity is checked in draw() after enumeration completes.
    if (current_page > 0) show_status_bar();
    return;
  }

  // MuPDF path (PDF, EPUB, XPS, ...)
  Log_Write(std::string("BookReader: opening via MuPDF: ") + path);

  // Lazy MuPDF init — deferred from startup to first PDF open
  if (ctx == NULL) {
    ctx = fz_new_context(NULL, NULL, 0);
    if (ctx) {
      fz_register_document_handlers(ctx);
      Log_Write("BookReader: initialized MuPDF context on first use");
    }
  }
  if (ctx == NULL) {
    Log_Error("BookReader: MuPDF context initialization failed");
    *result = -4;
    return;
  }

  fz_try(ctx) {
    doc = fz_open_document(ctx, path);

    if (!doc) {
      Log_Error(std::string("BookReader: fz_open_document returned null: ") +
                path);
      *result = -1;
      return;
    }

    int current_page = load_last_page(book_name.c_str());

    switch_current_page_layout(_currentPageLayout, current_page);

    if (!layout) {
      Log_Error(std::string("BookReader: MuPDF layout creation failed: ") +
                path);
      *result = -3;
      return;
    }

    Log_Write("BookReader: MuPDF opened OK, starting page=" +
              std::to_string(current_page));
    if (current_page > 0) show_status_bar();
    _total_pages = fz_count_pages(ctx, doc);
    if (_total_pages > 0)
      save_total_pages(book_name.c_str(), _total_pages);
  }
  fz_catch(ctx) {
    Log_Error(std::string("BookReader: fz_catch on open: ") + path);
    *result = -2;
    return;
  }
}

BookReader::~BookReader() {
  if (_nav_tex_left)  { SDL_DestroyTexture(_nav_tex_left);  _nav_tex_left  = nullptr; }
  if (_nav_tex_right) { SDL_DestroyTexture(_nav_tex_right); _nav_tex_right = nullptr; }
  if (_nav_tex_up)    { SDL_DestroyTexture(_nav_tex_up);    _nav_tex_up    = nullptr; }
  if (_nav_tex_down)  { SDL_DestroyTexture(_nav_tex_down);  _nav_tex_down  = nullptr; }

  if (doc) {
    fz_drop_document(ctx, doc);
    doc = nullptr;
  }

  if (layout) {
    delete layout;
    layout = nullptr;
  }

  if (config) {
    config_destroy(config);
    free(config);
    config = nullptr;
  }
}

void BookReader::previous_page(int n) {
  if (!layout) return;

  if (n == 1 && layout->current_page() == 0) {
    _chapter_change_request = -1;
    return;
  }

  layout->previous_page(n);
  show_status_bar();
  save_progress();
}


void BookReader::next_page(int n) {
  if (!layout) return;
  if (n == 1 &&
    _total_pages > 0 &&
    layout->current_page() >= _total_pages - 1) {
  _chapter_change_request = 1;
  return;
}

  int previous_page = layout->current_page();

  layout->next_page(n);

  int current_page = layout->current_page();

  register_manual_page(book_name.c_str(),
                       previous_page,
                       current_page,
                       _total_pages);

  show_status_bar();
  save_progress();
}

void BookReader::goto_page(int page_1indexed) {
  if (!layout) return;
  layout->goto_page(page_1indexed - 1);  // convert 1-indexed → 0-indexed
  show_status_bar();
  save_progress();
}

void BookReader::set_notes(const std::string &text) {
  _notes = text;
  notes_save(book_name.c_str(), _notes);
}

void BookReader::zoom_in(float zoom_amount) {
  if (!layout) return;
  layout->zoom_in(zoom_amount);
  show_status_bar();
}

void BookReader::zoom_out(float zoom_amount) {
  if (!layout) return;
  layout->zoom_out(zoom_amount);
  show_status_bar();
}

void BookReader::zoom_at_point(float delta, float px, float py) {
  if (!layout) return;
  if (_is_cbz)
    static_cast<CBZPageLayout*>(layout)->zoom_at_point(delta, px, py);
  else
    layout->zoom_in(delta);  // MuPDF: fallback без якоря
  show_status_bar();
}

void BookReader::move_page_up(int scroll_speed) {
  if (!layout || layout->pageFitsHeight()) return;
  layout->move_up(scroll_speed);
}

void BookReader::move_page_down(int scroll_speed) {
  if (!layout || layout->pageFitsHeight()) return;
  layout->move_down(scroll_speed);
}

void BookReader::move_page_left(int scroll_speed) {
  if (!layout || layout->pageFitsWidth()) return;
  layout->move_left(scroll_speed);
}

void BookReader::move_page_right(int scroll_speed) {
  if (!layout || layout->pageFitsWidth()) return;
  layout->move_right(scroll_speed);
}

void BookReader::reset_page() {
  if (!layout) return;
  layout->reset();
  show_status_bar();
}

void BookReader::zoom_max() {
  if (!layout) return;
  layout->zoom_max();
}

void BookReader::switch_page_layout() {
  // CBZ: Y button toggles single-page / two-page spread
 if (_is_cbz) {
    if (layout) {
        CBZPageLayout* cbz =
            static_cast<CBZPageLayout*>(layout);

        cbz->toggle_spread();

        _nav_landscape =
            cbz->isRotated();
    }

    return;
}

  switch (_currentPageLayout) {
    case BookPageLayoutPortrait:
      switch_current_page_layout(BookPageLayoutLandscape, 0);
      break;
    case BookPageLayoutLandscape:
      switch_current_page_layout(BookPageLayoutVertical, 0);
      break;
    case BookPageLayoutVertical:
      switch_current_page_layout(BookPageLayoutPortrait, 0);
      break;
  }
}

void BookReader::draw(bool drawHelp, bool drawNotes) {
  SDL_ClearScreen(RENDERER, BLACK);

  SDL_RenderClear(RENDERER);

  // Check if layout is valid
  if (!layout) {
    draw_centered_reader_message("Unable to open this page",
                                 "Return to the library and try again.");
    SDL_RenderPresent(RENDERER);
    return;
  }

  // CBZ: handle background archive enumeration state
  if (_is_cbz) {
    CBZPageLayout* cbz = static_cast<CBZPageLayout*>(layout);
    if (cbz->is_enumerating()) {
      // Show first page as soon as the enum thread has it ready.
      if (!cbz->is_valid() && cbz->is_first_image_ready())
        cbz->apply_first_image();
      if (cbz->is_valid()) {
        // First page is displayed while the remaining entries are indexed.
        layout->draw_page();
        SDL_Rect activity = {windowX - 46, windowY - 20, 28, 3};
        draw_rounded_rect(RENDERER, activity, 1, UI_ACCENT);
      } else {
        draw_centered_reader_message("Opening chapter",
                                     "Preparing pages for reading...");
      }
      SDL_RenderPresent(RENDERER);
      return;
    }
    if (cbz->is_enumerating_internal()) {
      // Enum thread done but finish_enumeration() not yet called
      cbz->finish_enumeration();
      if (!cbz->is_valid()) {
        // Archive had no images or couldn't be opened
        draw_centered_reader_message("No readable pages found",
                                     "Check the archive and try again.");
        SDL_RenderPresent(RENDERER);
        return;
      }
      // Persist total page count once — CBZ enum was async so constructor
      // couldn't do it. Guard on _total_pages == 0 ensures single write.
      if (_total_pages == 0) {
        _total_pages = cbz->page_count();
        if (_total_pages > 0)
          save_total_pages(book_name.c_str(), _total_pages);
      }
      if (
    _open_on_last_page &&
    _total_pages > 0
) {
    _open_on_last_page = false;

    goto_page(_total_pages);
}
    }
  }

  // Poll background work: MuPDF rasterization or CBZ prefetch-to-texture upload
  if (_is_cbz)
    static_cast<CBZPageLayout*>(layout)->poll_prefetch();
  else
    layout->poll_bg_render();

  layout->draw_page();
if (_total_pages > 0) {
    const bool rotated = _nav_landscape;

    const int margin = 12;
    const int thickness = 3;
    const int edge_distance = 8;

    const int available_length =
        (rotated ? windowY : windowX) -
        margin * 2;

    const int current_page =
        layout->current_page();

    const int gap =
        available_length / _total_pages >= 5
            ? 2
            : 1;

    for (int i = 0; i < _total_pages; i++) {
        const int segment_start =
            margin +
            (i * available_length) /
                _total_pages;

        const int segment_end =
            margin +
            ((i + 1) * available_length) /
                _total_pages;

        const int segment_length =
            std::max(
                1,
                segment_end -
                    segment_start -
                    gap
            );

        SDL_Rect segment;

        if (rotated) {
            segment = {
                edge_distance,
                segment_start,
                thickness,
                segment_length
            };
        } else {
            segment = {
                segment_start,
                windowY -
                    edge_distance -
                    thickness,
                segment_length,
                thickness
            };
        }

        const bool page_completed =
    configMangaMode
        ? i >= _total_pages - 1 - current_page
        : i <= current_page;

if (page_completed) {
            SDL_SetRenderDrawColor(
                RENDERER,
                UI_ACCENT.r,
                UI_ACCENT.g,
                UI_ACCENT.b,
                UI_ACCENT.a
            );
        } else {
            SDL_SetRenderDrawColor(
                RENDERER,
                49,
                49,
                54,
                255
            );
        }

        SDL_RenderFillRect(
            RENDERER,
            &segment
        );
    }
}
 if (drawHelp) {
    const int menu_width = 360;
    const int menu_height = 194;

    const int menu_x =
        (windowX - menu_width) / 2;

    const int menu_y =
        (windowY - menu_height) / 2;

    draw_reader_backdrop(84);

    SDL_Rect menu = {menu_x, menu_y, menu_width, menu_height};
    draw_rounded_rect(RENDERER, menu, 18, UI_SURFACE_ELEVATED);

   SDL_DrawText(
    RENDERER,
    ROBOTO_27,
    menu_x + 24,
    menu_y + 22,
    UI_TEXT,
    "Reading direction"
);

    SDL_DrawText(RENDERER, ROBOTO_15, menu_x + 25, menu_y + 57,
                 UI_TEXT_MUTED, "Choose how pages advance");

    for (int i = 0; i < 2; i++) {
        const bool selected =
            readingDirectionSelection == i;

        const int option_y =
            menu_y + 96 + i * 42;

        if (selected) {
            SDL_Rect selection = {
                menu_x + 14, option_y - 7, menu_width - 28, 36
            };
            draw_rounded_rect(RENDERER, selection, 9, UI_ACCENT_SOFT);
            SDL_Rect accent = {menu_x + 19, option_y - 2, 3, 26};
            draw_rounded_rect(RENDERER, accent, 1, UI_ACCENT);
        }

       SDL_DrawText(
    RENDERER,
    ROBOTO_20,
    menu_x + 30,
    option_y,
    selected
        ? UI_TEXT
        : UI_TEXT_MUTED,
    i == 0
        ? "Western"
        : "Eastern"
);
  }
}
  if (drawNotes) {
    const int noteWidth = 760;
    const int noteHeight = 448;
    const int noteX = (windowX - noteWidth) / 2;
    const int noteY = (windowY - noteHeight) / 2;

    draw_reader_backdrop(145);

    SDL_Rect notePanel = {noteX, noteY, noteWidth, noteHeight};
    draw_rounded_rect(RENDERER, notePanel, 20, UI_SURFACE_ELEVATED);

    SDL_DrawText(RENDERER, ROBOTO_30, noteX + 32, noteY + 25,
                 UI_TEXT, "Chapter notes");
    SDL_DrawText(RENDERER, ROBOTO_15, noteX + 33, noteY + 68,
                 UI_TEXT_MUTED, "Keep thoughts and references with this chapter");

    SDL_SetRenderDrawColor(RENDERER, UI_BORDER.r, UI_BORDER.g,
                           UI_BORDER.b, UI_BORDER.a);
    SDL_RenderDrawLine(RENDERER, noteX + 32, noteY + 108,
                       noteX + noteWidth - 32, noteY + 108);

    const int hintY = noteY + noteHeight - 40;
    SDL_DrawText(RENDERER, ROBOTO_15, noteX + 34, hintY,
                 UI_TEXT_MUTED, "A  Edit note       B  Close");

    const int bodyY = noteY + 134;
    const int bodyMaxH = hintY - bodyY - 18;
    if (_notes.empty()) {
      SDL_DrawText(RENDERER, ROBOTO_20, noteX + 34, bodyY,
                   UI_TEXT_MUTED, "No notes yet.");
      SDL_DrawText(RENDERER, ROBOTO_15, noteX + 35, bodyY + 35,
                   UI_TEXT_MUTED, "Press A to write something about this chapter.");
    } else {
      SDL_DrawTextWrapped(RENDERER, ROBOTO_20, noteX + 34, bodyY,
                          noteWidth - 68, bodyMaxH, UI_TEXT, _notes.c_str());
    }
  }

  if (configStatusBar && (permStatusBar || --status_bar_visible_counter > 0)) {
    char* title = layout->info();

    if (title && ROBOTO_15) {
      int title_width = 0, title_height = 0;
      TTF_SizeUTF8(ROBOTO_15, title, &title_width, &title_height);

      if (!_nav_landscape) {
        SDL_DrawRect(RENDERER, 0, 0, windowX, 40,
                     SDL_MakeColour(14, 14, 16, 205));
        SDL_DrawText(RENDERER, ROBOTO_15, (windowX - title_width) / 2,
                     (40 - title_height) / 2, UI_TEXT, title);

        StatusBar_DisplayTime(false);
      } else {
        SDL_DrawRect(RENDERER, windowX - 40, 0, 40, windowY,
                     SDL_MakeColour(14, 14, 16, 205));
        int x = (windowX - title_width) - ((40 - title_height) / 2);
        int y = (windowY - title_height) / 2;
        SDL_DrawRotatedText(RENDERER, ROBOTO_15, 90.0, x, y, UI_TEXT,
                            title);

        StatusBar_DisplayTime(true);
      }
    }
  }

  if (configScreenButtons && !drawHelp && !drawNotes) {
    uint32_t now = SDL_GetTicks();
    if (now < _btn_hide_at) {
      uint32_t ms_left = _btn_hide_at - now;
      int alpha = (ms_left < 600u) ? (int)(ms_left * 200u / 600u) : 200;

      auto make_nav = [&](const char* utf8) -> SDL_Texture* {
        if (!ROBOTO_35) return nullptr;
        SDL_Surface* s = TTF_RenderUTF8_Blended(
            ROBOTO_35, utf8, SDL_MakeColour(255, 255, 255, 255));
        if (!s) return nullptr;
        SDL_Texture* t = SDL_CreateTextureFromSurface(RENDERER, s);
        SDL_FreeSurface(s);
        return t;
      };
      if (!_nav_tex_init) {
        _nav_tex_left  = make_nav("\xe2\x80\xb9");
        _nav_tex_right = make_nav("\xe2\x80\xba");
        _nav_tex_up    = make_nav("\xe2\x86\x91");
        _nav_tex_down  = make_nav("\xe2\x86\x93");
        _nav_tex_init  = true;
      }

      const int BW = 68, BH = 68;
      auto draw_btn = [&](int bx, int by, SDL_Texture* glyph, double angle = 0.0) {
        SDL_Rect bg = {bx, by, BW, BH};
        draw_rounded_rect(RENDERER, bg, 16,
                          SDL_Color{18, 18, 21, (Uint8)alpha});
        if (glyph) {
          SDL_SetTextureAlphaMod(glyph, (Uint8)alpha);
          SDL_SetTextureBlendMode(glyph, SDL_BLENDMODE_BLEND);
          int tw = 0, th = 0;
          SDL_QueryTexture(glyph, NULL, NULL, &tw, &th);
          SDL_Rect dst = {bx + (BW - tw) / 2, by + (BH - th) / 2, tw, th};
          SDL_RenderCopyEx(RENDERER, glyph, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
        }
        SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_NONE);
      };

      if (!_nav_landscape) {
        draw_btn(18,                    (windowY - BH) / 2, _nav_tex_left);
        draw_btn(windowX - 18 - BW,     (windowY - BH) / 2, _nav_tex_right);
      } else {
        // Rotate ‹/› 90° so they point up/down — avoids font missing ↑/↓ glyphs
        draw_btn((windowX - BW) / 2, 18,                 _nav_tex_left,  90.0);
        draw_btn((windowX - BW) / 2, windowY - 18 - BH, _nav_tex_right, 90.0);
      }
    }
  }
  if (!_chapter_notice.empty() &&
      SDL_GetTicks() < _chapter_notice_until) {
    SDL_Surface* notice_surface = TTF_RenderUTF8_Blended(
        ROBOTO_20, _chapter_notice.c_str(), UI_TEXT);

    if (notice_surface) {
      const int text_width = notice_surface->w;
      const int text_height = notice_surface->h;
      SDL_Texture* notice_texture =
          SDL_CreateTextureFromSurface(RENDERER, notice_surface);
      SDL_FreeSurface(notice_surface);

      if (notice_texture) {
        const bool rotated = _nav_landscape;
        const int notice_width =
            rotated ? text_height + 36 : text_width + 52;
        const int notice_height =
            rotated ? text_width + 52 : text_height + 30;
        const int notice_x =
            rotated ? windowX - notice_width - 20
                    : (windowX - notice_width) / 2;
        const int notice_y =
            rotated ? (windowY - notice_height) / 2 : 24;

        SDL_Rect notice_rect = {
            notice_x, notice_y, notice_width, notice_height};
        draw_rounded_rect(RENDERER, notice_rect, 12,
                          SDL_Color{23, 23, 26, 232});

        SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(RENDERER, UI_ACCENT.r, UI_ACCENT.g,
                               UI_ACCENT.b, UI_ACCENT.a);
        if (!rotated) {
          SDL_Rect accent = {notice_x + 12, notice_y + 9,
                             3, notice_height - 18};
          SDL_RenderFillRect(RENDERER, &accent);
        } else {
          SDL_Rect accent = {notice_x + 9, notice_y + 12,
                             notice_width - 18, 3};
          SDL_RenderFillRect(RENDERER, &accent);
        }

        SDL_Rect text_rect = {
            notice_x + (notice_width - text_width) / 2,
            notice_y + (notice_height - text_height) / 2,
            text_width,
            text_height};
        SDL_RenderCopyEx(RENDERER, notice_texture, nullptr, &text_rect,
                         rotated ? 90.0 : 0.0, nullptr, SDL_FLIP_NONE);
        SDL_DestroyTexture(notice_texture);
        SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_NONE);
      }
    }
  }
  SDL_RenderPresent(RENDERER);
}

void BookReader::show_status_bar() { status_bar_visible_counter = 200; }
void BookReader::reset_nav_buttons() { _btn_hide_at = SDL_GetTicks() + 3000; }
void BookReader::show_chapter_notice(const std::string& text) {
  _chapter_notice = text;
  _chapter_notice_until = SDL_GetTicks() + 2000;
}

void BookReader::save_progress() {
  if (!layout) return;
  save_last_page(book_name.c_str(), layout->current_page());
  // Total pages is invariant while a book is open; written once at open time.
  // Removed from here to eliminate the double SD write per page navigation.
}

void BookReader::switch_current_page_layout(BookPageLayout bookPageLayout,
                                            int current_page) {
  if (layout) {
    current_page = layout->current_page();
    delete layout;
    layout = nullptr;
  }

  _currentPageLayout = bookPageLayout;
  _nav_landscape     = (bookPageLayout != BookPageLayoutPortrait);

  // CBZ: use CBZPageLayout (no MuPDF doc involved, landscape not yet supported)
  if (_is_cbz) {
    CBZPageLayout* cbz = new CBZPageLayout(book_path.c_str(), current_page);
    // Always assign layout — enumeration may still be running in background.
    // BookReader::draw() will call finish_enumeration() when is_enumerating() = false.
    layout = cbz;
    return;
  }

  // MuPDF path
  fz_try(ctx) {
    switch (bookPageLayout) {
      case BookPageLayoutPortrait:
        layout = new PageLayout(doc, current_page);
        break;
      case BookPageLayoutLandscape:
        layout = new LandscapePageLayout(doc, current_page);
        break;
      case BookPageLayoutVertical:
        layout = new VerticalPageLayout(doc, current_page);
        break;
    }
  }
  fz_catch(ctx) {
    Log_Error("BookReader: fz_catch creating MuPDF layout for page " +
              std::to_string(current_page));
    layout = nullptr;
  }

  // Enable background rasterization now that the layout is constructed.
  if (layout && !book_path.empty())
    layout->set_bg_path(book_path.c_str());
}
