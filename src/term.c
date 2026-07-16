/* Terminal and mouse glue over the shared Kitty framebuffer presenter. */
#include "fishtank.h"
#include "kitty_framebuffer.h"

#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static bool framebuffer_active;
static volatile int shutdown_claimed;
static int cell_width = 9, cell_height = 18;
static int origin_column = 1, origin_row = 1;

static void update_mouse_geometry(int width, int height)
{
    struct winsize window;
    int columns = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_col > 0) columns = window.ws_col;
        if (window.ws_row > 0) rows = window.ws_row;
    }
    cell_width = kittyfb_cell_width(&framebuffer);
    cell_height = kittyfb_cell_height(&framebuffer);
    int image_columns = (width + cell_width - 1) / cell_width;
    int image_rows = (height + cell_height - 1) / cell_height;
    origin_column = 1 + (columns - image_columns) / 2;
    origin_row = 1 + ((rows - 1) - image_rows) / 2;
    if (origin_column < 1) origin_column = 1;
    if (origin_row < 1) origin_row = 1;
}

bool term_init(int *outW, int *outH)
{
    kittyfb_options options;

    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&options);
    options.install_winch_handler = false;
    options.enter_sequence = "\x1b[?1003h\x1b[?1006h";
    options.leave_sequence = "\x1b[?1003l\x1b[?1000l\x1b[?1006l";
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO,
                      &options) != 0)
        return false;
    framebuffer_active = true;
    shutdown_claimed = 0;
    *outW = kittyfb_width(&framebuffer);
    *outH = kittyfb_height(&framebuffer);
    update_mouse_geometry(*outW, *outH);
    return true;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    if (framebuffer_active)
        (void)kittyfb_present(&framebuffer, rgba, w, h);
}

static bool claim_shutdown(void)
{
    if (!framebuffer_active) return false;
    return !__sync_lock_test_and_set(&shutdown_claimed, 1);
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    if (!claim_shutdown()) return;
    kittyfb_emergency_restore(&framebuffer);
}

static int parse_sgr_mouse(void)
{
    char buffer[64];
    int length = 0;
    char final = 0;
    while (length < (int)sizeof buffer - 1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) return -1;
        buffer[length++] = (char)c;
        if (c == 'M' || c == 'm') { final = (char)c; break; }
    }
    if (!final) return -1;
    buffer[length - 1] = '\0';

    int button = 0, column = 0, row = 0;
    if (sscanf(buffer, "%d;%d;%d", &button, &column, &row) != 3) return -1;
    int x = (column - origin_column) * cell_width;
    int y = (row - origin_row) * cell_height;
    if (G.W > 0) x = (int)clampf((float)x, 0.0f, (float)(G.W - 1));
    if (G.H > 0) y = (int)clampf((float)y, 0.0f, (float)(G.H - 1));
    G.mouseX = x;
    G.mouseY = y;
    if (final == 'M' && (button & 3) == 0 && !(button & 32)) return KEY_MOUSE;
    return KEY_MOUSE_MOVE;
}

int term_poll_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c == '\t') return KEY_TAB;
    if (c == 3) { G.quit = true; return -1; }
    if (c == 0x1b) {
        unsigned char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return KEY_ESC;
        if (seq[0] != '[' && seq[0] != 'O') return KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return KEY_ESC;
        switch (seq[1]) {
        case '<': return parse_sgr_mouse();
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:
            while (seq[1] >= '0' && seq[1] <= ';')
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;
            return -1;
        }
    }
    return c;
}
