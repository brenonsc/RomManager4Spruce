// rmkey - RomManager's button reader.
//   rmkey -d REQ RESP   stays running: keeps the pad open and queues presses, so none are
//                       missed while the screen draws. Lines on REQ: "get MS" (MS -1 =
//                       wait for ever) answers one key name on RESP, or "none" after MS;
//                       "flush" forgets queued presses; "quit".
//   rmkey -t MS         waits up to MS for one press and prints it
//   rmkey               waits for one press and prints it
// Names: up down left right A B X Y L R L2 R2 start select menu.
// Reads spruce's virtual pad ("TRIMUI Player1", the TrimUI Brick / Brick Pro / Smart Pro)
// or, without one, every input device - RMKEY_DEV=/dev/input/eventN picks one.
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { UP, DOWN, LEFT, RIGHT, A, B, X, Y, L, R, L2, R2, START, SELECT, MENU, NKEYS };
static const char *names[NKEYS] = {"up", "down", "left", "right", "A", "B", "X", "Y",
                                   "L", "R", "L2", "R2", "start", "select", "menu"};

// Held directions repeat after this long, this often (only while nothing is queued, so a
// slow screen never builds a backlog of moves)
#define REPEAT_DELAY 350
#define REPEAT_EVERY 90

static int pad_only; // the named pad: its keyboard-style codes would double the buttons
static int held[NKEYS];
static long held_since[NKEYS], last_repeat;
static int queue[16], qn;
static int fds[16], nfds;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void push(int k) {
    if (qn == 16) memmove(queue, queue + 1, --qn * sizeof *queue);
    queue[qn++] = k;
}

static void set(int k, int down) {
    if (down && !held[k]) {
        push(k);
        held_since[k] = now_ms();
    }
    held[k] = down;
}

static int btn(int code) {
    switch (code) {
    case BTN_EAST: return A; // spruce's TrimUI pad: A is 305, B 304, X 308, Y 307
    case BTN_SOUTH: return B;
    case BTN_WEST: return X;
    case BTN_NORTH: return Y;
    case BTN_TL: return L;
    case BTN_TR: return R;
    case BTN_TL2: return L2;
    case BTN_TR2: return R2;
    case BTN_SELECT: return SELECT;
    case BTN_START: return START;
    case BTN_MODE: return MENU;
    case BTN_DPAD_UP: return UP;
    case BTN_DPAD_DOWN: return DOWN;
    case BTN_DPAD_LEFT: return LEFT;
    case BTN_DPAD_RIGHT: return RIGHT;
    }
    if (pad_only) return -1;
    switch (code) { // keyboards, and the Miyoo Mini's layout
    case KEY_UP: return UP;
    case KEY_DOWN: return DOWN;
    case KEY_LEFT: return LEFT;
    case KEY_RIGHT: return RIGHT;
    case KEY_SPACE: return A;
    case KEY_LEFTCTRL: return B;
    case KEY_LEFTSHIFT: return X;
    case KEY_LEFTALT: return Y;
    case KEY_E: return L;
    case KEY_T: return R;
    case KEY_TAB: return L2;
    case KEY_BACKSPACE: return R2;
    case KEY_ENTER: return START;
    case KEY_RIGHTCTRL: return SELECT;
    case KEY_ESC: return MENU;
    }
    return -1;
}

static void axis(int lo, int hi, int v, int press, int release) {
    // press past +-press, let go inside +-release (so a stick resting near the edge
    // doesn't flicker)
    if (v <= -press) set(lo, 1);
    else if (v > -release) set(lo, 0);
    if (v >= press) set(hi, 1);
    else if (v < release) set(hi, 0);
}

static void event(struct input_event *e) {
    if (e->type == EV_KEY && e->value != 2) {
        int k = btn(e->code);
        if (k >= 0) set(k, e->value != 0);
    } else if (e->type == EV_ABS) {
        switch (e->code) {
        case ABS_HAT0X: axis(LEFT, RIGHT, e->value, 1, 1); break;
        case ABS_HAT0Y: axis(UP, DOWN, e->value, 1, 1); break;
        case ABS_X: axis(LEFT, RIGHT, e->value, 16000, 8000); break; // left stick
        case ABS_Y: axis(UP, DOWN, e->value, 16000, 8000); break;
        case ABS_Z: set(L2, e->value > 100); break; // triggers: 0..255
        case ABS_RZ: set(R2, e->value > 100); break;
        }
    }
}

// The pad's event node from /proc/bus/input/devices, as spruce finds it
static int named_pad(char *path, size_t n) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;
    char line[512];
    int in = 0, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "N: Name=", 8)) in = strstr(line, "TRIMUI Player1") != NULL;
        else if (in && !strncmp(line, "H: Handlers=", 12)) {
            char *ev = strstr(line, "event");
            if (ev) found = snprintf(path, n, "/dev/input/%.*s", (int)strspn(ev, "event0123456789"), ev) > 0;
        }
    }
    fclose(f);
    return found;
}

static void open_inputs(void) {
    char path[300];
    const char *dev = getenv("RMKEY_DEV");
    if ((dev && *dev && snprintf(path, sizeof path, "%s", dev)) || (pad_only = named_pad(path, sizeof path))) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { fds[nfds++] = fd; return; }
        pad_only = 0;
    }
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && nfds < 16) {
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) fds[nfds++] = fd;
    }
    if (d) closedir(d);
}

static void read_inputs(struct pollfd *p) {
    struct input_event ev[32];
    int gone = 0;
    for (int i = 0; i < nfds; i++) {
        if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { // unplugged: stop watching it
            close(fds[i]);
            fds[i] = -1, gone = 1;
            continue;
        }
        if (!(p[i].revents & POLLIN)) continue;
        ssize_t n;
        while ((n = read(fds[i], ev, sizeof ev)) > 0)
            for (size_t j = 0; j < n / sizeof *ev; j++) event(&ev[j]);
    }
    if (gone) { // (the caller rebuilds its poll list from fds)
        int k = 0;
        for (int i = 0; i < nfds; i++)
            if (fds[i] >= 0) fds[k++] = fds[i];
        nfds = k;
    }
}

// The held direction due to repeat now, or -1; *wait: ms until the next one
static int repeat_due(long *wait) {
    long t = now_ms();
    *wait = -1;
    for (int k = UP; k <= RIGHT; k++) {
        if (!held[k]) continue;
        long due = held_since[k] + REPEAT_DELAY;
        if (last_repeat + REPEAT_EVERY > due) due = last_repeat + REPEAT_EVERY;
        if (due <= t) { last_repeat = t; return k; }
        if (*wait < 0 || due - t < *wait) *wait = due - t;
    }
    return -1;
}

// Waits up to ms (-1: for ever) for a press
static int wait_key(long ms) {
    long end = ms < 0 ? -1 : now_ms() + ms;
    for (;;) {
        if (qn) {
            int k = queue[0];
            memmove(queue, queue + 1, --qn * sizeof *queue);
            return k;
        }
        long rep, left = end < 0 ? -1 : end - now_ms();
        int k = repeat_due(&rep);
        if (k >= 0) return k;
        if (end >= 0 && left <= 0) return -1;
        long t = left < 0 ? rep : (rep < 0 || left < rep ? left : rep);
        struct pollfd p[16];
        for (int i = 0; i < nfds; i++) p[i] = (struct pollfd){fds[i], POLLIN, 0};
        if (poll(p, nfds, (int)t) > 0) read_inputs(p);
    }
}

// Between requests: reads the pad (presses queue up) until ctl has a line
static void pump(int ctl) {
    struct pollfd p[17];
    for (;;) {
        for (int i = 0; i < nfds; i++) p[i] = (struct pollfd){fds[i], POLLIN, 0};
        p[nfds] = (struct pollfd){ctl, POLLIN, 0};
        if (poll(p, nfds + 1, -1) <= 0) continue;
        int ctl_ready = p[nfds].revents & POLLIN; // (read_inputs may drop devices)
        read_inputs(p);
        if (ctl_ready) return;
    }
}

static int daemon_mode(const char *req, const char *resp) {
    int in = open(req, O_RDWR), out = open(resp, O_RDWR);
    if (in < 0 || out < 0) { perror("rmkey"); return 1; }
    char buf[256];
    size_t len = 0;
    for (;;) {
        char *nl;
        while (!(nl = memchr(buf, '\n', len))) {
            pump(in);
            ssize_t n = read(in, buf + len, sizeof buf - 1 - len);
            if (n <= 0) return 0;
            len += n;
        }
        *nl = 0;
        if (!strncmp(buf, "get", 3)) {
            int k = wait_key(atol(buf + 3 + (buf[3] == ' ')));
            dprintf(out, "%s\n", k >= 0 ? names[k] : "none");
        } else if (!strcmp(buf, "flush")) {
            qn = 0;
        } else if (!strcmp(buf, "quit")) {
            return 0;
        }
        len -= nl + 1 - buf;
        memmove(buf, nl + 1, len);
    }
}

int main(int argc, char **argv) {
    open_inputs();
    if (!nfds) { fprintf(stderr, "rmkey: no input device\n"); return 1; }
    if (argc == 4 && !strcmp(argv[1], "-d")) return daemon_mode(argv[2], argv[3]);
    long ms = (argc == 3 && !strcmp(argv[1], "-t")) ? atol(argv[2]) : -1;
    int k = wait_key(ms);
    if (k >= 0) puts(names[k]);
    return 0;
}
