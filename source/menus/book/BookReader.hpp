#ifndef WOOK_READER_BOOK_READER_HPP
#define WOOK_READER_BOOK_READER_HPP

#include <mupdf/pdf.h>
#include <string>
#include "PageLayout.hpp"
#include "CBZPageLayout.hpp"
#include <switch.h>
struct SDL_Texture;

typedef enum
{
    BookPageLayoutPortrait,
    BookPageLayoutLandscape,
    BookPageLayoutVertical
} BookPageLayout;

class BookReader
{
public:
    BookReader(const char *path, int *result);
    ~BookReader();

    bool permStatusBar = false;
    int readingDirectionSelection = 0;

    void previous_page(int n);
    void next_page(int n);
    void goto_page(int page_1indexed);
    void zoom_in(float zoom_amount);
    void zoom_out(float zoom_amount);
    void zoom_at_point(float delta, float px, float py);
    void move_page_up(int scroll_speed);
    void move_page_down(int scroll_speed);
    void move_page_left(int scroll_speed);
    void move_page_right(int scroll_speed);
    void reset_page();
    void zoom_max();
    void switch_page_layout();
    void draw(bool drawHelp, bool drawNotes = false);
    void reset_nav_buttons();
void show_chapter_notice(const std::string& text);

    const std::string &notes() const { return _notes; }
    void set_notes(const std::string &text);

    BookPageLayout currentPageLayout() { return _currentPageLayout; }
    bool navLandscape()                { return _nav_landscape; }
int takeChapterChangeRequest() {
  int request = _chapter_change_request;
  _chapter_change_request = 0;
  return request;
}

private:
    void show_status_bar();
    void save_progress();
    void switch_current_page_layout(BookPageLayout bookPageLayout, int current_page);

    fz_document *doc = NULL;
    int status_bar_visible_counter = 0;
    uint32_t _btn_hide_at = 0;
    bool _nav_tex_init = false;
    SDL_Texture* _nav_tex_left  = nullptr;
    SDL_Texture* _nav_tex_right = nullptr;
    SDL_Texture* _nav_tex_up    = nullptr;
    SDL_Texture* _nav_tex_down  = nullptr;

    BookPageLayout _currentPageLayout = BookPageLayoutPortrait;
    bool _nav_landscape = false;
    PageLayout *layout = nullptr;

    std::string book_name;
    std::string book_path; // needed to reconstruct CBZPageLayout on layout switch
    bool _is_cbz = false;
    int  _total_pages = 0;
int _chapter_change_request = 0;
std::string _chapter_notice;
uint32_t _chapter_notice_until = 0;
    std::string _notes;
};

#endif
