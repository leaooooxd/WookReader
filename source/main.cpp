#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include <sys/stat.h>
#include <unwind.h>

#include <ctime>
#include <fstream>
#include <iostream>

#ifdef DEBUG
#include <twili.h>
#endif

extern "C" {
#include "MenuChooser.h"
#include "SDL_helper.h"
#include "common.h"
#include "config.h"
#include "fs.h"
#include "menu_book_reader.h"
#include "textures.h"
}
#include <mupdf/fitz.h>

// Application log. Keep the existing SD-card location for compatibility with
// the user's books, reading progress, and preferences.
#define LOG_FILE "/switch/WookReader/log.txt"

std::ofstream logFile;

void Log_Init() {
  logFile.open(LOG_FILE, std::ios::out | std::ios::trunc);
  if (logFile.is_open()) {
    logFile << "=== NeetReader Log Started ===" << std::endl;
    logFile.flush();
  }
}

void Log_Write(const std::string& msg) {
  if (logFile.is_open()) {
    logFile << "[LOG] " << msg << std::endl;
    logFile.flush();
  }
}

void Log_Error(const std::string& msg) {
  if (logFile.is_open()) {
    logFile << "[ERROR] " << msg << std::endl;
    logFile.flush();
  }
}

void Log_Close() {
  if (logFile.is_open()) {
    logFile << "=== NeetReader Log Ended ===" << std::endl;
    logFile.close();
  }
}
// Stack traces make unexpected termination actionable without changing the
// normal startup or rendering paths.
extern "C" void _start(void);

struct BtState {
  void** cur;
  void** end;
};
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg) {
  BtState* s = static_cast<BtState*>(arg);
  if (s->cur == s->end) return _URC_END_OF_STACK;
  uintptr_t pc = _Unwind_GetIP(ctx);
  *s->cur++ = reinterpret_cast<void*>(pc);
  return _URC_NO_REASON;
}

static void log_stack_trace() {
  void* buf[32];
  BtState st = {buf, buf + 32};
  _Unwind_Backtrace(bt_cb, &st);
  size_t n = st.cur - buf;
  char line[64];
  snprintf(line, sizeof(line), "load_base=0x%lx",
           (unsigned long)(uintptr_t)_start);
  Log_Error(line);
  for (size_t i = 0; i < n; i++) {
    snprintf(line, sizeof(line), "  [%02zu] 0x%016lx", i,
             (unsigned long)(uintptr_t)buf[i]);
    Log_Error(line);
  }
}

static void terminate_handler() {
  Log_Error("TERMINATE: uncaught exception — see stack trace below");
  log_stack_trace();
  Log_Close();
  exit(1);
}
fz_context* ctx = nullptr;
bool timeInitialized = false;
bool psmInitialized = false;
bool nifmInitialized = false;

SDL_Renderer* RENDERER = nullptr;
SDL_Window* WINDOW = nullptr;
SDL_Event EVENT;
TTF_Font* ROBOTO_35 = nullptr;
TTF_Font* ROBOTO_30 = nullptr;
TTF_Font* ROBOTO_27 = nullptr;
TTF_Font* ROBOTO_25 = nullptr;
TTF_Font* ROBOTO_20 = nullptr;
TTF_Font* ROBOTO_15 = nullptr;
bool configDarkMode = true;
bool configScreenButtons = false;
bool configStatusBar = true;
bool configMangaMode = false;

void Term_Services() {
  Log_Write("Shutting down NeetReader");

  if (nifmInitialized) {
    nifmExit();
    nifmInitialized = false;
  }

  if (psmInitialized) {
    psmExit();
    psmInitialized = false;
  }

  if (timeInitialized) {
    timeExit();
    timeInitialized = false;
  }
  SDL_TextCache_Clear();  // must be before TTF_CloseFont
  TTF_CloseFont(ROBOTO_35);
  TTF_CloseFont(ROBOTO_30);
  TTF_CloseFont(ROBOTO_27);
  TTF_CloseFont(ROBOTO_25);
  TTF_CloseFont(ROBOTO_20);
  TTF_CloseFont(ROBOTO_15);
  TTF_Quit();

  if (ctx) {
    fz_drop_context(ctx);
    ctx = nullptr;
  }

  Textures_Free();
  romfsExit();

  IMG_Quit();

  SDL_DestroyRenderer(RENDERER);
  SDL_DestroyWindow(WINDOW);
  SDL_Quit();

#ifdef DEBUG
  twiliExit();
#endif

  Log_Close();
}

bool Init_Services() {
#ifdef DEBUG
  twiliInitialize();
#endif

  // Preserve the original one-time migration and existing WookReader data path.
  {
    struct stat st_old, st_new;
    bool old_exists =
        (stat("/switch/eBookReader", &st_old) == 0 && S_ISDIR(st_old.st_mode));
    bool new_exists =
        (stat("/switch/WookReader", &st_new) == 0 && S_ISDIR(st_new.st_mode));
    if (old_exists && !new_exists)
      rename("/switch/eBookReader", "/switch/WookReader");
  }

  FS_RecursiveMakeDir("/switch/WookReader");
  Log_Init();
  Log_Write("Starting NeetReader");

  romfsInit();
  Log_Write("Initialized RomFs");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
    Log_Error(std::string("SDL_Init failed: ") + SDL_GetError());
    Term_Services();
    return false;
  }
  Log_Write("Initialized SDL");

  if (R_SUCCEEDED(timeInitialize())) {
    timeInitialized = true;
    Log_Write("Initialized clock service");
  } else {
    Log_Error("Clock service unavailable");
  }

  if (R_SUCCEEDED(psmInitialize())) {
    psmInitialized = true;
    Log_Write("Initialized battery service");
  } else {
    Log_Error("Battery service unavailable");
  }

  if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
    nifmInitialized = true;
    Log_Write("Initialized network service");
  } else {
    Log_Error("Network service unavailable");
  }

  if (SDL_CreateWindowAndRenderer(1280, 720, 0, &WINDOW, &RENDERER) == -1) {
    Log_Error(std::string("SDL_CreateWindowAndRenderer failed: ") +
              SDL_GetError());
    Term_Services();
    return false;
  }
  SDL_SetWindowTitle(WINDOW, "NeetReader");
  Log_Write("Initialized Window and Renderer");

  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");

  if (!IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG)) {
    Log_Error(std::string("IMG_Init failed: ") + IMG_GetError());
    Term_Services();
    return false;
  }
  Log_Write("Initialized Image");

  // MuPDF context deferred to first PDF/EPUB/XPS open (BookReader.cpp)
  Log_Write("MuPDF init deferred to first use");

  if (TTF_Init() == -1) {
    Log_Error(std::string("TTF_Init failed: ") + TTF_GetError());
    Term_Services();
    return false;
  }
  Log_Write("Initialized TTF");

  Textures_Load();
  Log_Write("Loaded Textures");

  const char* medium_font = "romfs:/resources/font/Geist-Medium.otf";
  const char* regular_font = "romfs:/resources/font/Geist-Regular.otf";

  ROBOTO_35 = TTF_OpenFont(medium_font, 32);
  ROBOTO_30 = TTF_OpenFont(medium_font, 28);
  ROBOTO_27 = TTF_OpenFont(medium_font, 25);
  ROBOTO_25 = TTF_OpenFont(regular_font, 23);
  ROBOTO_20 = TTF_OpenFont(regular_font, 19);
  ROBOTO_15 = TTF_OpenFont(regular_font, 15);

  if (!ROBOTO_35 || !ROBOTO_30 || !ROBOTO_27 || !ROBOTO_25 || !ROBOTO_20 ||
      !ROBOTO_15) {
    Log_Error(std::string("Failed to load Geist interface fonts: ") +
              TTF_GetError());
    Term_Services();
    return false;
  }

  TTF_Font* interface_fonts[] = {
      ROBOTO_35, ROBOTO_30, ROBOTO_27,
      ROBOTO_25, ROBOTO_20, ROBOTO_15};
  for (TTF_Font* font : interface_fonts) {
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    TTF_SetFontKerning(font, 1);
  }
  Log_Write("Initialized Geist interface fonts");

  for (int i = 0; i < 2; i++) {
    if (SDL_JoystickOpen(i) == NULL) {
      Log_Error(std::string("SDL_JoystickOpen failed: ") + SDL_GetError());
      Term_Services();
      return false;
    }
  }
  Log_Write("Initialized Input");

  FS_RecursiveMakeDir("/switch/WookReader/books");
  Log_Write("Created book directory if needed");

  Log_Write("NeetReader initialization complete");
  return true;
}

int main(int argc, char* argv[]) {
  std::set_terminate(terminate_handler);

  if (!Init_Services()) return 1;

  Log_Write("argc = " + std::to_string(argc));
  for (int i = 0; i < argc; i++) {
    Log_Write("argv[" + std::to_string(i) + "] = " + std::string(argv[i]));
  }

  if (argc == 2) {
    Log_Write("Opening book: " + std::string(argv[1]));
    Menu_OpenBook(argv[1], 3, 0.3);
  } else {
    Log_Write("Starting file chooser");
    Menu_StartChoosing();
  }

  Term_Services();
  return 0;
}
