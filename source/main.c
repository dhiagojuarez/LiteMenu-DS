/**
 * @file main.c
 * @brief LiteMenu - Lightweight Nintendo DS Lite Slot-1 Flashcard Frontend
 *
 * Designed specifically for DS Lite (NTR-001 / USG-001) operating within strict
 * 4MB Main RAM constraints (0x02000000 - 0x023FFFFF) on ARM946E-S (67 MHz).
 * Optimized for R4, DSTT, and Ace3DS+ Slot-1 flashcards via DLDI & libfat.
 *
 * Architectural Highlights:
 * - Zero dynamic heap allocation (malloc/free avoided) in the directory browser
 *   to eliminate heap fragmentation across deep directory traversals.
 * - Fixed-window streaming pagination: Only `PAGE_SIZE` (16) entries are kept
 *   in static BSS memory at any time. Navigating pages streams entries directly
 *   from the FAT table via `opendir()` / `readdir()`.
 * - Strict VRAM mapping:
 *     - Main Engine (Top Screen): VRAM_A mapped to BG0 (Text UI) & BG2 (2D Header/Frame)
 *     - Sub Engine (Bottom Screen): VRAM_C mapped to Sub BG0 (Touch Targets, Metadata & Debug Console)
 * - Dual-stage ROM Launch Subsystem:
 *     - Primary: Standard `argv` handoff (`runNdsFile`) to `/_nds/nds-bootstrap.nds`,
 *       `/YSMenu.nds`, or direct homebrew `.nds` DLDI execution.
 *     - Emulator routing: Automatically resolves `.gb`/`.gbc` to `/_nds/gameyob.nds`
 *       and `.nes` to `/_nds/nesDS.nds` via `argv[0]`/`argv[1]` chains.
 */

#include <nds.h>
#include <nds/arm9/dldi.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* =========================================================================
 * 1. HARDWARE & MEMORY CONSTANTS (4MB NTR MODE BUDGET)
 * ========================================================================= */

#define NTR_RAM_BASE        0x02000000U
#define NTR_RAM_LIMIT       0x02400000U  /* Strictly 4MB (never touch DSi 0x02400000+) */

#define PAGE_SIZE           14           /* Entries visible per page on Top Screen */
#define MAX_FILENAME_LEN    128          /* Bounded filename buffer per visible row */
#define MAX_PATH_LEN        256          /* Bounded working directory path buffer */
#define MAX_DIR_DEPTH       16           /* Bounded directory history stack depth */

/* Touch UI button bounding boxes on Bottom Screen (256x192) */
#define BTN_LAUNCH_X1       16
#define BTN_LAUNCH_Y1       136
#define BTN_LAUNCH_X2       120
#define BTN_LAUNCH_Y2       176

#define BTN_BACK_X1         136
#define BTN_BACK_Y1         136
#define BTN_BACK_X2         240
#define BTN_BACK_Y2         176

#define BTN_PREV_X1         16
#define BTN_PREV_Y1         96
#define BTN_PREV_X2         120
#define BTN_PREV_Y2         126

#define BTN_NEXT_X1         136
#define BTN_NEXT_Y1         96
#define BTN_NEXT_X2         240
#define BTN_NEXT_Y2         126

/* File Row Touch Zone on Bottom Screen (Quick-Select Rows) */
#define TOUCH_LIST_TOP_Y    32
#define TOUCH_LIST_ROW_H    12

/* =========================================================================
 * 2. DATA STRUCTURES (STATICALLY ALLOCATED IN .BSS - ~3.2 KB TOTAL)
 * ========================================================================= */

typedef enum {
    ENTRY_TYPE_DIR = 0,
    ENTRY_TYPE_NDS,
    ENTRY_TYPE_GB,
    ENTRY_TYPE_GBC,
    ENTRY_TYPE_NES,
    ENTRY_TYPE_UNKNOWN
} EntryType;

typedef struct {
    char      name[MAX_FILENAME_LEN];
    uint32_t  size_bytes;
    EntryType type;
    char      game_title[13]; /* 12-char NDS header title + null terminator */
    char      game_code[5];   /* 4-char NDS game code + null terminator */
} FileEntry;

typedef struct {
    bool      use_ysmenu_fallback; /* Toggle between nds-bootstrap and YSMenu */
    bool      show_hidden_files;   /* Toggle dotfile visibility */
    bool      vblank_led_blink;    /* Visual heartbeat indicator */
    int       backlight_level;     /* DS Lite PMIC brightness (0..3) */
} FrontendSettings;

/* Packed Nintendo DS ROM Header (first 16 bytes needed for inspection) */
typedef struct __attribute__((packed)) {
    char     title[12];
    char     gamecode[4];
} NdsShortHeader;

/* Static BSS state - zero heap usage */
static FileEntry        g_page_entries[PAGE_SIZE];
static char             g_cwd[MAX_PATH_LEN] = "fat:/";
static uint16_t         g_parent_cursor_stack[MAX_DIR_DEPTH];
static uint8_t          g_dir_depth = 0;

static int              g_total_matching_entries = 0;
static int              g_current_page           = 0;
static int              g_entries_on_page        = 0;
static int              g_cursor_in_page         = 0;
static bool             g_show_detail_modal      = false;
static bool             g_fat_mounted            = false;
static char             g_status_msg[64]         = "Ready.";

static FrontendSettings g_settings = {
    .use_ysmenu_fallback = false,
    .show_hidden_files   = false,
    .vblank_led_blink    = false,
    .backlight_level     = 2
};

/* Dual PrintConsole contexts for Top (Main) and Bottom (Sub) screens */
static PrintConsole     g_top_console;
static PrintConsole     g_sub_console;

/* Custom 2D background layer ID for Main Screen decorative accent bar */
static int              g_bg2_main_id = -1;

/* =========================================================================
 * 3. FORWARD DECLARATIONS
 * ========================================================================= */

static void      initHardwareAndVRAM(void);
static void      drawTopAccentBarVRAM(void);
static bool      initFileSystemAndDLDI(void);
static EntryType classifyFileExtension(const char *filename);
static bool      shouldIncludeEntry(const char *name, bool is_dir);
static void      inspectNdsHeader(const char *full_path, FileEntry *entry);
static int       countFilteredDirectoryEntries(const char *dir_path);
static void      loadDirectoryPage(const char *dir_path, int page_index);
static void      navigateIntoDirectory(const char *subdir_name);
static void      navigateUpDirectory(void);
static void      renderTopScreenBrowser(void);
static void      renderBottomScreenDashboard(void);
static void      renderFatalErrorScreen(const char *stage, const char *detail, int err_code);
static int       launchSelectedEntry(const FileEntry *entry);
static int       bootPayloadWithArgv(const char *loader_path, const char *rom_path);
static void      cycleBacklightBrightness(void);
static void      handleInputAndTouch(uint32_t keys_down, uint32_t keys_repeat, touchPosition touch);

/* =========================================================================
 * 4. HARDWARE, MEMORY & VRAM INITIALIZATION
 * ========================================================================= */

/**
 * @brief Configures ARM946E-S hardware mode, strictly maps VRAM banks A and C,
 *        and initializes dual-screen text/graphics engines.
 */
static void initHardwareAndVRAM(void) {
    /* Turn on 2D engines on both Main (Top) and Sub (Bottom) displays */
    powerOn(POWER_ALL_2D);

    /* Ensure Top Screen is Main Engine and Bottom Screen is Sub Engine */
    lcdMainOnTop();

    /*
     * Strict VRAM Allocation Plan:
     * - VRAM_A (128 KB): Mapped to Main Engine BG0 (Console Text) & BG2 (16-bit Bitmap Header/Footer)
     * - VRAM_C (128 KB): Mapped to Sub Engine BG0 (Bottom Screen Console & Touch UI)
     * - VRAM_B, D, E, F, G, H, I: Unmapped/Reserved to keep power and bus contention minimal
     */
    vramSetPrimaryBanks(
        VRAM_A_MAIN_BG,
        VRAM_B_LCD,
        VRAM_C_SUB_BG,
        VRAM_D_LCD
    );

    /* Configure Main Engine: Mode 5 (Text BG0 + Extended Bitmap BG2) */
    videoSetMode(MODE_5_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG2_ACTIVE);

    /* Configure Sub Engine: Mode 0 (Text BG0) */
    videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE);

    /* Initialize Top Screen Console on Main BG0 (Tile base 0, Map base 30) */
    consoleInit(&g_top_console,
                0,                       /* Layer 0 (BG0) */
                BgType_Text4bpp,
                BgSize_T_256x256,
                30,                      /* Map Base (offset 60KB in VRAM_A) */
                0,                       /* Tile Base (offset 0KB in VRAM_A) */
                true,                    /* Main Engine (Top Screen) */
                true);                   /* Load default 1bpp font */

    /* Initialize Bottom Screen Console on Sub BG0 (Tile base 0, Map base 31) */
    consoleInit(&g_sub_console,
                0,                       /* Layer 0 (Sub BG0) */
                BgType_Text4bpp,
                BgSize_T_256x256,
                31,                      /* Map Base */
                0,                       /* Tile Base */
                false,                   /* Sub Engine (Bottom Screen) */
                true);                   /* Load default 1bpp font */

    /* Initialize Main BG2 as a 256x256 16-bit direct color bitmap atBmpBase(2) (offset 32KB in VRAM_A) */
    g_bg2_main_id = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 2, 0);
    bgSetPriority(0, 0);                 /* BG0 (Text) highest priority (on top) */
    bgSetPriority(g_bg2_main_id, 2);     /* BG2 (Bitmap backdrop) behind text */

    drawTopAccentBarVRAM();
}

/**
 * @brief Renders a lightweight 16-bit RGB555 header/footer backdrop into Main BG2 VRAM.
 *        Uses DMA-safe 16-bit writes directly to VRAM_A bitmap region.
 */
static void drawTopAccentBarVRAM(void) {
    if (g_bg2_main_id < 0) return;

    uint16_t *vram_bmp = bgGetGfxPtr(g_bg2_main_id);
    if (!vram_bmp) return;

    const uint16_t col_header = RGB15(2, 6, 15)  | BIT(15); /* Deep Navy Blue */
    const uint16_t col_body   = RGB15(1, 2, 4)   | BIT(15); /* Dark Slate */
    const uint16_t col_footer = RGB15(3, 8, 12)  | BIT(15); /* Steel Blue */

    for (int y = 0; y < 192; y++) {
        uint16_t row_color = col_body;
        if (y < 16) {
            row_color = col_header;
        } else if (y >= 176) {
            row_color = col_footer;
        }
        for (int x = 0; x < 256; x++) {
            vram_bmp[y * 256 + x] = row_color;
        }
    }
}

/* =========================================================================
 * 5. SLOT-1 DLDI VERIFICATION & FAT FILESYSTEM INITIALIZATION
 * ========================================================================= */

/**
 * @brief Validates the active Slot-1 DLDI driver stub and mounts the FAT/FAT32
 *        partition via `fatInitDefault()`.
 */
static bool initFileSystemAndDLDI(void) {
    const DLDI_INTERFACE *dldi = io_dldi_data;

    /* Verify DLDI Magic Number (0xBF8DA5ED) */
    if (dldi == NULL || dldi->magicNumber != DLDI_MAGIC_NUMBER) {
        renderFatalErrorScreen(
            "DLDI VALIDATION FAILED",
            "Invalid DLDI magic signature in ARM9 binary.",
            -1
        );
        return false;
    }

    /* Ensure the flashcard driver reports Slot-1 SD/MMC read capability */
    if ((dldi->ioInterface.features & FEATURE_MEDIUM_CANREAD) == 0) {
        renderFatalErrorScreen(
            "DLDI DRIVER UNPATCHED",
            "Default no-op DLDI stub detected. Patch with dlditool or auto-DLDI flashcard.",
            (int)dldi->ioInterface.features
        );
        return false;
    }

    /* Initialize libfat with default cache size (safe for 4MB DS Lite RAM) */
    if (!fatInitDefault()) {
        renderFatalErrorScreen(
            "FAT/FAT32 MOUNT FAILED",
            "fatInitDefault() returned false. Check MicroSD MBR & FAT32 cluster size.",
            errno
        );
        return false;
    }

    /* Set initial working directory */
    if (chdir("fat:/") != 0) {
        /* Fallback to root "/" if fat:/ prefix is aliased */
        strncpy(g_cwd, "/", sizeof(g_cwd) - 1);
        chdir("/");
    }

    g_fat_mounted = true;
    snprintf(g_status_msg, sizeof(g_status_msg), "DLDI: %.28s", dldi->friendlyName);
    return true;
}

/* =========================================================================
 * 6. HIGH-EFFICIENCY PAGINATED DIRECTORY SCANNER (ZERO HEAP OVERHEAD)
 * ========================================================================= */

/**
 * @brief Matches supported ROM/emulator extensions case-insensitively.
 */
static EntryType classifyFileExtension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return ENTRY_TYPE_UNKNOWN;
    }

    if (strcasecmp(dot, ".nds") == 0 || strcasecmp(dot, ".ids") == 0) {
        return ENTRY_TYPE_NDS;
    }
    if (strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".sgb") == 0) {
        return ENTRY_TYPE_GB;
    }
    if (strcasecmp(dot, ".gbc") == 0) {
        return ENTRY_TYPE_GBC;
    }
    if (strcasecmp(dot, ".nes") == 0 || strcasecmp(dot, ".fds") == 0) {
        return ENTRY_TYPE_NES;
    }
    return ENTRY_TYPE_UNKNOWN;
}

/**
 * @brief Determines whether a directory entry should be included in the browser.
 */
static bool shouldIncludeEntry(const char *name, bool is_dir) {
    if (!name || name[0] == '\0') return false;

    /* Always skip "." and ".." pseudo-entries (parent navigation uses B button) */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    /* Filter hidden dotfiles / system volume folders unless enabled */
    if (!g_settings.show_hidden_files) {
        if (name[0] == '.') return false;
        if (strcasecmp(name, "System Volume Information") == 0) return false;
    }

    if (is_dir) {
        return true;
    }

    return (classifyFileExtension(name) != ENTRY_TYPE_UNKNOWN);
}

/**
 * @brief Reads the first 16 bytes of a `.nds` file to extract the embedded
 *        12-byte Game Title and 4-byte Game Code without loading the whole header.
 */
static void inspectNdsHeader(const char *full_path, FileEntry *entry) {
    memset(entry->game_title, 0, sizeof(entry->game_title));
    memset(entry->game_code, 0, sizeof(entry->game_code));

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        strncpy(entry->game_title, "UNREADABLE", sizeof(entry->game_title) - 1);
        strncpy(entry->game_code, "----", sizeof(entry->game_code) - 1);
        return;
    }

    NdsShortHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) {
        memcpy(entry->game_title, hdr.title, 12);
        entry->game_title[12] = '\0';
        /* Sanitize non-printable characters */
        for (int i = 0; i < 12; i++) {
            if (entry->game_title[i] != '\0' &&
                (entry->game_title[i] < 32 || entry->game_title[i] > 126)) {
                entry->game_title[i] = ' ';
            }
        }

        memcpy(entry->game_code, hdr.gamecode, 4);
        entry->game_code[4] = '\0';
        for (int i = 0; i < 4; i++) {
            if (entry->game_code[i] < 32 || entry->game_code[i] > 126) {
                entry->game_code[i] = '?';
            }
        }
    }
    fclose(fp);
}

/**
 * @brief Counts total filtered entries in `dir_path` in a single streaming pass.
 *        Requires O(1) memory regardless of whether the folder has 10 or 5,000 ROMs.
 */
static int countFilteredDirectoryEntries(const char *dir_path) {
    DIR *pdir = opendir(dir_path);
    if (!pdir) {
        return -1;
    }

    int count = 0;
    struct dirent *pent;
    struct stat st;
    char full_path[MAX_PATH_LEN];

    while ((pent = readdir(pdir)) != NULL) {
        bool is_dir = (pent->d_type == DT_DIR);

        if (pent->d_type == DT_UNKNOWN) {
            snprintf(full_path, sizeof(full_path), "%s%s%s",
                     dir_path,
                     (dir_path[strlen(dir_path) - 1] == '/') ? "" : "/",
                     pent->d_name);
            if (stat(full_path, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }
        }

        if (shouldIncludeEntry(pent->d_name, is_dir)) {
            count++;
        }
    }

    closedir(pdir);
    return count;
}

/**
 * @brief Streams only the requested slice `[page_index * PAGE_SIZE .. (page_index + 1) * PAGE_SIZE - 1]`
 *        into the static `g_page_entries` window. Two-pass streaming ensures directories
 *        always appear before ROM files without allocating sorting buffers on the heap.
 */
static void loadDirectoryPage(const char *dir_path, int page_index) {
    memset(g_page_entries, 0, sizeof(g_page_entries));
    g_entries_on_page = 0;

    g_total_matching_entries = countFilteredDirectoryEntries(dir_path);
    if (g_total_matching_entries < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "ERR: Cannot open %s", dir_path);
        g_total_matching_entries = 0;
        return;
    }

    int total_pages = (g_total_matching_entries + PAGE_SIZE - 1) / PAGE_SIZE;
    if (total_pages <= 0) total_pages = 1;

    if (page_index >= total_pages) page_index = total_pages - 1;
    if (page_index < 0) page_index = 0;
    g_current_page = page_index;

    const int start_offset = g_current_page * PAGE_SIZE;
    int logical_index = 0;

    char full_path[MAX_PATH_LEN];
    struct stat st;

    /*
     * Pass 0: Stream subdirectories first
     * Pass 1: Stream matching ROM files (.nds, .gb, .gbc, .nes) second
     */
    for (int pass = 0; pass < 2 && g_entries_on_page < PAGE_SIZE; pass++) {
        DIR *pdir = opendir(dir_path);
        if (!pdir) break;

        struct dirent *pent;
        while ((pent = readdir(pdir)) != NULL && g_entries_on_page < PAGE_SIZE) {
            bool is_dir = (pent->d_type == DT_DIR);

            snprintf(full_path, sizeof(full_path), "%s%s%s",
                     dir_path,
                     (dir_path[strlen(dir_path) - 1] == '/') ? "" : "/",
                     pent->d_name);

            bool stat_valid = false;
            if (pent->d_type == DT_UNKNOWN) {
                if (stat(full_path, &st) == 0) {
                    stat_valid = true;
                    is_dir = S_ISDIR(st.st_mode);
                }
            }

            if ((pass == 0 && !is_dir) || (pass == 1 && is_dir)) {
                continue;
            }

            if (!shouldIncludeEntry(pent->d_name, is_dir)) {
                continue;
            }

            if (logical_index >= start_offset && g_entries_on_page < PAGE_SIZE) {
                FileEntry *slot = &g_page_entries[g_entries_on_page];
                strncpy(slot->name, pent->d_name, MAX_FILENAME_LEN - 1);
                slot->name[MAX_FILENAME_LEN - 1] = '\0';

                if (is_dir) {
                    slot->type = ENTRY_TYPE_DIR;
                    slot->size_bytes = 0;
                    strncpy(slot->game_title, "<FOLDER>", sizeof(slot->game_title) - 1);
                    strncpy(slot->game_code, "DIR ", sizeof(slot->game_code) - 1);
                } else {
                    slot->type = classifyFileExtension(pent->d_name);
                    if (!stat_valid && stat(full_path, &st) == 0) {
                        stat_valid = true;
                    }
                    slot->size_bytes = stat_valid ? (uint32_t)st.st_size : 0;

                    if (slot->type == ENTRY_TYPE_NDS) {
                        inspectNdsHeader(full_path, slot);
                    } else if (slot->type == ENTRY_TYPE_GB || slot->type == ENTRY_TYPE_GBC) {
                        strncpy(slot->game_title, "GAMEYOB ROM", sizeof(slot->game_title) - 1);
                        strncpy(slot->game_code, "DMG ", sizeof(slot->game_code) - 1);
                    } else if (slot->type == ENTRY_TYPE_NES) {
                        strncpy(slot->game_title, "NESDS ROM", sizeof(slot->game_title) - 1);
                        strncpy(slot->game_code, "NES ", sizeof(slot->game_code) - 1);
                    }
                }
                g_entries_on_page++;
            }
            logical_index++;
        }
        closedir(pdir);
    }

    if (g_cursor_in_page >= g_entries_on_page) {
        g_cursor_in_page = (g_entries_on_page > 0) ? (g_entries_on_page - 1) : 0;
    }
}

/**
 * @brief Descends into a selected child directory, pushing parent cursor state.
 */
static void navigateIntoDirectory(const char *subdi