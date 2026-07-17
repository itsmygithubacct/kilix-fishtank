/* Terminal and mouse glue over the shared Kitty framebuffer presenter. */
#include "fishtank.h"
#include "kitty_framebuffer.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static kittykb_terminal keyboard;
static bool framebuffer_active;
static bool keyboard_active;
static volatile int shutdown_claimed;
static int cell_width = 9, cell_height = 18;
static int origin_column = 1, origin_row = 1;
static int demux_state;
static int64_t demux_pending_since;
static char mouse_sequence[64];
static size_t mouse_length;
static int mouse_events[16];
static size_t mouse_head, mouse_count;

enum { DEMUX_NORMAL, DEMUX_ESCAPE, DEMUX_CSI, DEMUX_MOUSE };

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

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
    kittykb_terminal_options key_options;

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
    kittykb_terminal_init(&keyboard);
    kittykb_terminal_options_init(&key_options);
    key_options.flags = KITTYKB_FLAGS_KEY_STATE;
    key_options.make_raw = false;
    key_options.make_nonblocking = false;
    if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                               &key_options) != 0) {
        int error = errno;
        kittyfb_stop(&framebuffer);
        framebuffer_active = false;
        errno = error;
        return false;
    }
    keyboard_active = true;
    demux_state = DEMUX_NORMAL;
    mouse_length = mouse_head = mouse_count = 0;
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
    if (keyboard_active) {
        (void)kittykb_terminal_stop(&keyboard);
        keyboard_active = false;
    }
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    static const char keyboard_pop[] = "\x1b\\\x1b[<u";

    if (!claim_shutdown()) return;
    if (keyboard_active)
        (void)write(STDOUT_FILENO, keyboard_pop, sizeof keyboard_pop - 1);
    kittyfb_emergency_restore(&framebuffer);
}

static int parse_sgr_mouse(char final)
{
    mouse_sequence[mouse_length] = '\0';

    int button = 0, column = 0, row = 0;
    if (sscanf(mouse_sequence, "%d;%d;%d", &button, &column, &row) != 3)
        return -1;
    int x = (column - origin_column) * cell_width;
    int y = (row - origin_row) * cell_height;
    if (G.W > 0) x = (int)clampf((float)x, 0.0f, (float)(G.W - 1));
    if (G.H > 0) y = (int)clampf((float)y, 0.0f, (float)(G.H - 1));
    G.mouseX = x;
    G.mouseY = y;
    if (final == 'M' && (button & 3) == 0 && !(button & 32)) return KEY_MOUSE;
    return KEY_MOUSE_MOVE;
}

static void queue_mouse_event(int key)
{
    if (key < 0 || mouse_count == sizeof mouse_events / sizeof mouse_events[0])
        return;
    mouse_events[(mouse_head + mouse_count) %
                 (sizeof mouse_events / sizeof mouse_events[0])] = key;
    mouse_count++;
}

static void demux_byte(unsigned char byte)
{
    static const unsigned char escape = 0x1b;
    static const unsigned char csi[] = {0x1b, '['};

    switch (demux_state) {
    case DEMUX_NORMAL:
        if (byte == escape) {
            demux_state = DEMUX_ESCAPE;
            demux_pending_since = monotonic_milliseconds();
        } else {
            kittykb_input_feed(&keyboard.input, &byte, 1);
        }
        break;
    case DEMUX_ESCAPE:
        if (byte == '[') {
            demux_state = DEMUX_CSI;
        } else {
            kittykb_input_feed(&keyboard.input, &escape, 1);
            kittykb_input_feed(&keyboard.input, &byte, 1);
            demux_state = DEMUX_NORMAL;
        }
        break;
    case DEMUX_CSI:
        if (byte == '<') {
            mouse_length = 0;
            demux_state = DEMUX_MOUSE;
        } else {
            kittykb_input_feed(&keyboard.input, csi, sizeof csi);
            kittykb_input_feed(&keyboard.input, &byte, 1);
            demux_state = DEMUX_NORMAL;
        }
        break;
    case DEMUX_MOUSE:
        if (byte == 'M' || byte == 'm') {
            queue_mouse_event(parse_sgr_mouse((char)byte));
            mouse_length = 0;
            demux_state = DEMUX_NORMAL;
        } else if (mouse_length + 1 < sizeof mouse_sequence) {
            mouse_sequence[mouse_length++] = (char)byte;
        } else {
            mouse_length = 0;
            demux_state = DEMUX_NORMAL;
        }
        break;
    }
}

static void read_input(void)
{
    unsigned char bytes[256];
    ssize_t count;
    while ((count = read(STDIN_FILENO, bytes, sizeof bytes)) > 0)
        for (ssize_t index = 0; index < count; index++)
            demux_byte(bytes[index]);

    if ((demux_state == DEMUX_ESCAPE || demux_state == DEMUX_CSI) &&
        monotonic_milliseconds() - demux_pending_since >= 25) {
        static const unsigned char pending_csi[] = {0x1b, '['};
        size_t count_to_feed = demux_state == DEMUX_ESCAPE ? 1 : 2;
        kittykb_input_feed(&keyboard.input, pending_csi, count_to_feed);
        if (demux_state == DEMUX_ESCAPE)
            kittykb_input_flush_escape(&keyboard.input);
        demux_state = DEMUX_NORMAL;
    }
}

static int game_key(uint32_t key)
{
    switch (key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    default: return key <= 0x7fU ? (int)key : -1;
    }
}

int term_poll_key(void)
{
    kittykb_event event;
    if (!keyboard_active) return -1;
    read_input();
    if (mouse_count > 0) {
        int key = mouse_events[mouse_head];
        mouse_head = (mouse_head + 1) %
                     (sizeof mouse_events / sizeof mouse_events[0]);
        mouse_count--;
        return key;
    }
    while (kittykb_input_next(&keyboard.input, &event)) {
        if (event.action == KITTYKB_ACTION_RELEASE) continue;
        if ((event.modifiers & KITTYKB_MOD_CTRL) &&
            (event.key == 'c' || event.key == 'C')) {
            G.quit = true;
            return -1;
        }
        int key = game_key(event.key);
        if (key >= 0) return key;
    }
    return -1;
}
