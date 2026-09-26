#include "aap.h"
#include "drivers.h"
#include "shell.h"

#define AAP_W 48
#define AAP_H 12
#define AAP_FRAMES 8
#define CANVAS_X 4
#define CANVAS_Y 5
#define FRAME_X 58
#define FRAME_Y 5

static char frames[AAP_FRAMES][AAP_W * AAP_H];
static int cur_frame;
static int cx, cy;
static int loaded;

static void put(int r, int c, char ch, u8 attr) {
    char s[2];
    s[0] = ch; s[1] = 0;
    vga_write_at((u8)r, (u8)c, s, attr);
}

static void text(int r, int c, const char *s, u8 attr) {
    vga_write_at((u8)r, (u8)c, s, attr);
}

static void fill(int r, int c, int n, char ch, u8 attr) {
    char s[80];
    if (n > 79) n = 79;
    for (int i = 0; i < n; i++) s[i] = ch;
    s[n] = 0;
    text(r, c, s, attr);
}

static void clear_frames(void) {
    for (int f = 0; f < AAP_FRAMES; f++)
        for (int i = 0; i < AAP_W * AAP_H; i++)
            frames[f][i] = ' ';
    cur_frame = cx = cy = 0;
}

static void draw_ui(void) {
    vga_clear();
    vga_setcolor(0x07);

    fill(0, 0, 80, ' ', 0x70);
    text(0, 2, "AAP  ASCII ANIMATION EDITOR", 0x70);
    text(0, 61, "ESC exit", 0x70);

    text(2, 4, "+------------------------------------------------+", 0x0B);
    text(3, 4, "|                    CANVAS                      |", 0x0B);
    text(4, 4, "+------------------------------------------------+", 0x0B);
    for (int y = 0; y < AAP_H; y++) {
        put(CANVAS_Y + y, CANVAS_X - 1, '|', 0x0B);
        put(CANVAS_Y + y, CANVAS_X + AAP_W, '|', 0x0B);
        for (int x = 0; x < AAP_W; x++) {
            char ch = frames[cur_frame][y * AAP_W + x];
            put(CANVAS_Y + y, CANVAS_X + x, ch, 0x07);
        }
    }
    text(CANVAS_Y + AAP_H, 4, "+------------------------------------------------+", 0x0B);

    fill(2, FRAME_X, 19, ' ', 0x70);
    text(2, FRAME_X, "FRAMES", 0x70);
    for (int f = 0; f < AAP_FRAMES; f++) {
        char b[8];
        b[0] = '['; b[1] = '0' + f; b[2] = ']'; b[3] = 0;
        text(FRAME_Y + f, FRAME_X, b, f == cur_frame ? 0x70 : 0x07);
        text(FRAME_Y + f, FRAME_X + 4, f == cur_frame ? "< editing" : "        ",
             f == cur_frame ? 0x70 : 0x07);
    }

    text(16, FRAME_X, "TOOLS", 0x70);
    text(17, FRAME_X, "Arrows  move", 0x07);
    text(18, FRAME_X, "Keys    draw", 0x07);
    text(19, FRAME_X, "Del     erase", 0x07);
    text(20, FRAME_X, "N/B     frame", 0x07);
    text(21, FRAME_X, "D       duplicate", 0x07);
    text(22, FRAME_X, "C       clear", 0x07);
    text(23, FRAME_X, "P/S/L   preview/save/load", 0x07);

    text(19, 4, "FRAME:", 0x0B);
    put(19, 11, '0' + cur_frame, 0x0B);
    text(20, 4, "CURSOR:", 0x0B);
    text(20, 12, cx < 10 ? " " : "", 0x0B);
    put(20, 12, '0' + (cx / 10), 0x0B);
    put(20, 13, '0' + (cx % 10), 0x0B);
    text(20, 15, "x", 0x0B);
    put(20, 17, '0' + (cy / 10), 0x0B);
    put(20, 18, '0' + (cy % 10), 0x0B);

    put(CANVAS_Y + cy, CANVAS_X + cx, frames[cur_frame][cy * AAP_W + cx],
        0x70);
}

static void draw_cell(void) {
    char ch = frames[cur_frame][cy * AAP_W + cx];
    put(CANVAS_Y + cy, CANVAS_X + cx, ch, 0x70);
}

static void restore_cell(int x, int y) {
    put(CANVAS_Y + y, CANVAS_X + x, frames[cur_frame][y * AAP_W + x], 0x07);
}

static void save_frames(void) {
    char path[16];
    shell_mkdir("/aap");
    for (int f = 0; f < AAP_FRAMES; f++) {
        path[0] = '/'; path[1] = 'a'; path[2] = 'a'; path[3] = 'p'; path[4] = '/';
        path[5] = 'f'; path[6] = '0' + f; path[7] = 0;
        shell_fwrite(path, frames[f], AAP_W * AAP_H);
    }
    text(1, 4, "saved to /aap/f0 .. /aap/f7", 0x0A);
}

static void load_frames(void) {
    char path[16];
    for (int f = 0; f < AAP_FRAMES; f++) {
        path[0] = '/'; path[1] = 'a'; path[2] = 'a'; path[3] = 'p'; path[4] = '/';
        path[5] = 'f'; path[6] = '0' + f; path[7] = 0;
        int n = shell_fread(path, frames[f], AAP_W * AAP_H + 1);
        if (n == AAP_W * AAP_H) loaded = 1;
    }
    if (loaded) text(1, 4, "loaded /aap animation", 0x0A);
    else text(1, 4, "no saved animation found", 0x0C);
}

static void duplicate_frame(void) {
    int next = (cur_frame + 1) % AAP_FRAMES;
    for (int i = 0; i < AAP_W * AAP_H; i++)
        frames[next][i] = frames[cur_frame][i];
    cur_frame = next;
}

static void preview(void) {
    for (int f = 0; f < AAP_FRAMES; f++) {
        vga_clear();
        fill(0, 0, 80, ' ', 0x70);
        text(0, 2, "AAP  PREVIEW", 0x70);
        text(0, 66, "ESC after preview", 0x70);
        text(2, 4, "+------------------------------------------------+", 0x0B);
        for (int y = 0; y < AAP_H; y++) {
            put(CANVAS_Y + y, CANVAS_X - 1, '|', 0x0B);
            put(CANVAS_Y + y, CANVAS_X + AAP_W, '|', 0x0B);
            for (int x = 0; x < AAP_W; x++)
                put(CANVAS_Y + y, CANVAS_X + x, frames[f][y * AAP_W + x], 0x07);
        }
        text(CANVAS_Y + AAP_H, 4, "+------------------------------------------------+", 0x0B);
        text(20, 4, "frame ", 0x0B);
        put(20, 10, '0' + f, 0x0B);
        sleep_ms(180);
    }
}

int aap_run(void) {
    clear_frames();
    loaded = 0;
    load_frames();
    draw_ui();

    for (;;) {
        int k = kbd_getkey();
        if (k == 27) {
            vga_clear();
            return 0;
        }

        restore_cell(cx, cy);

        if (k == KEY_LEFT && cx > 0) cx--;
        else if (k == KEY_RIGHT && cx < AAP_W - 1) cx++;
        else if (k == KEY_UP && cy > 0) cy--;
        else if (k == KEY_DOWN && cy < AAP_H - 1) cy++;
        else if (k == KEY_DEL || k == '\b') {
            frames[cur_frame][cy * AAP_W + cx] = ' ';
        } else if (k == 'n' || k == 'N') {
            cur_frame = (cur_frame + 1) % AAP_FRAMES;
        } else if (k == 'b' || k == 'B') {
            cur_frame = (cur_frame + AAP_FRAMES - 1) % AAP_FRAMES;
        } else if (k == 'd' || k == 'D') {
            duplicate_frame();
        } else if (k == 'c' || k == 'C') {
            for (int i = 0; i < AAP_W * AAP_H; i++) frames[cur_frame][i] = ' ';
        } else if (k == 'p' || k == 'P') {
            preview();
        } else if (k == 's' || k == 'S') {
            save_frames();
        } else if (k == 'l' || k == 'L') {
            load_frames();
        } else if (k >= 32 && k < 127) {
            frames[cur_frame][cy * AAP_W + cx] = (char)k;
            if (cx < AAP_W - 1) cx++;
        }
        draw_ui();
    }
}
