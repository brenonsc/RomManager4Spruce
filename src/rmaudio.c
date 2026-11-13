// rmaudio - RomManager's sound mixer: the music on a loop with the button sounds mixed
// over it (so a beep never pauses the music), as one stream of 16-bit stereo 44.1 kHz
// PCM on stdout. launch.sh hands it to spruce's ffmpeg, which plays it through ALSA.
//   rmaudio -d FIFO [music.mp3] [-o -]
// Lines on FIFO: "bgm on" / "bgm off", "vol music N" / "vol sfx N" (0-10), "sfx FILE.wav",
// "quit".
// The stream is written in step with the clock, only LEAD_MS ahead: the player's buffers
// are big, and filling them would make every button sound late.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3_ex.h"

#define RATE 44100
#define CHUNK 441    // frames per write: 10 ms
#define LEAD_MS 70   // how far ahead of the clock the stream runs
#define VOICES 8
#define SOUNDS 8

typedef struct { int16_t *pcm; long frames; } Sound; // stereo, RATE
typedef struct { const Sound *s; long pos; } Voice;

static Sound music;
static long music_pos;
static int music_on, music_vol = 10, sfx_vol = 10;
static int music_gain; // current, ramped toward the target (Q16) to avoid clicks
static struct { char path[512]; Sound s; } sounds[SOUNDS];
static int nsounds;
static Voice voices[VOICES];

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

// Any rate / mono or stereo 16-bit samples -> stereo RATE (linear interpolation)
static Sound convert(const int16_t *in, long frames, int ch, int hz) {
    Sound s = {0};
    if (frames <= 0 || ch < 1 || hz <= 0) return s;
    s.frames = (long)((long long)frames * RATE / hz);
    s.pcm = malloc(s.frames * 4);
    for (long i = 0; i < s.frames; i++) {
        long long p = (long long)i * hz * 256 / RATE; // source position, 8 fractional bits
        long a = p >> 8, b = a + 1 < frames ? a + 1 : a;
        int f = p & 255;
        for (int c = 0; c < 2; c++) {
            int cc = c < ch ? c : 0;
            s.pcm[i * 2 + c] = (int16_t)((in[a * ch + cc] * (256 - f) + in[b * ch + cc] * f) >> 8);
        }
    }
    return s;
}

static Sound load_mp3(const char *path) {
    mp3dec_t dec;
    mp3dec_file_info_t info = {0};
    Sound s = {0};
    if (mp3dec_load(&dec, path, &info, NULL, NULL) || !info.samples) return s;
    s = convert(info.buffer, info.samples / info.channels, info.channels, info.hz);
    free(info.buffer);
    return s;
}

// PCM .wav, 8 or 16 bit
static Sound load_wav(const char *path) {
    Sound s = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    unsigned char h[12], ck[8];
    int ch = 0, hz = 0, bits = 0;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        while (fread(ck, 1, 8, f) == 8) {
            uint32_t n = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                unsigned char fm[16];
                if (n < 16 || fread(fm, 1, 16, f) != 16) break;
                ch = fm[2] | fm[3] << 8;
                hz = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
                bits = fm[14] | fm[15] << 8;
                fseek(f, n - 16 + (n & 1), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4) && ch && (bits == 16 || bits == 8)) {
                unsigned char *raw = malloc(n);
                n = fread(raw, 1, n, f);
                long frames = n / (bits / 8) / ch;
                int16_t *pcm = malloc(frames * ch * 2 + 2);
                for (long i = 0; i < frames * ch; i++)
                    pcm[i] = bits == 16 ? (int16_t)(raw[i * 2] | raw[i * 2 + 1] << 8) : (int16_t)((raw[i] - 128) << 8);
                s = convert(pcm, frames, ch, hz);
                free(pcm);
                free(raw);
                break;
            } else {
                fseek(f, n + (n & 1), SEEK_CUR);
            }
        }
    }
    fclose(f);
    return s;
}

static void play(const char *path) {
    const Sound *s = NULL;
    for (int i = 0; i < nsounds && !s; i++)
        if (!strcmp(sounds[i].path, path)) s = &sounds[i].s;
    if (!s) {
        if (nsounds == SOUNDS) return;
        snprintf(sounds[nsounds].path, sizeof sounds[0].path, "%s", path);
        sounds[nsounds].s = load_wav(path);
        s = &sounds[nsounds++].s;
    }
    if (!s->frames) return;
    int v = 0; // a free voice, or the one furthest along
    for (int i = 0; i < VOICES; i++) {
        if (!voices[i].s) { v = i; break; }
        if (voices[i].pos > voices[v].pos) v = i;
    }
    voices[v] = (Voice){s, 0};
}

static int gain(int vol) { return vol * vol * 65536 / 100; } // squared: the steps sound even

static void mix(int16_t *out) {
    int target = music_on && music.frames ? gain(music_vol) : 0, sg = gain(sfx_vol);
    for (int i = 0; i < CHUNK; i++) {
        // ~10 ms fade when the music starts or stops
        if (music_gain < target) music_gain = music_gain + 150 > target ? target : music_gain + 150;
        if (music_gain > target) music_gain = music_gain - 150 < target ? target : music_gain - 150;
        int32_t l = 0, r = 0;
        if (music_gain && music.frames) {
            l = music.pcm[music_pos * 2] * (int64_t)music_gain >> 16;
            r = music.pcm[music_pos * 2 + 1] * (int64_t)music_gain >> 16;
            if (++music_pos >= music.frames) music_pos = 0;
        }
        for (int v = 0; v < VOICES; v++) {
            Voice *vo = &voices[v];
            if (!vo->s) continue;
            l += vo->s->pcm[vo->pos * 2] * (int64_t)sg >> 16;
            r += vo->s->pcm[vo->pos * 2 + 1] * (int64_t)sg >> 16;
            if (++vo->pos >= vo->s->frames) vo->s = NULL;
        }
        out[i * 2] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        out[i * 2 + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
}

static int command(char *line) {
    if (!strcmp(line, "bgm on")) music_on = 1;
    else if (!strcmp(line, "bgm off")) music_on = 0, music_pos = 0;
    else if (!strncmp(line, "vol music ", 10)) music_vol = atoi(line + 10);
    else if (!strncmp(line, "vol sfx ", 8)) sfx_vol = atoi(line + 8);
    else if (!strncmp(line, "sfx ", 4)) play(line + 4);
    else if (!strcmp(line, "quit")) return 0;
    if (music_vol < 0 || music_vol > 10) music_vol = 10;
    if (sfx_vol < 0 || sfx_vol > 10) sfx_vol = 10;
    return 1;
}

static int write_all(const int16_t *p, size_t n) {
    const char *b = (const char *)p;
    while (n) {
        ssize_t w = write(1, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        b += w;
        n -= w;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *fifo = NULL, *music_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) fifo = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) i++; // always stdout
        else music_path = argv[i];
    }
    if (!fifo) { fprintf(stderr, "usage: rmaudio -d FIFO [music.mp3] [-o -]\n"); return 2; }
    int ctl = open(fifo, O_RDWR | O_NONBLOCK);
    if (ctl < 0) { fprintf(stderr, "rmaudio: can't open %s: %s\n", fifo, strerror(errno)); return 1; }
    if (isatty(1)) { fprintf(stderr, "rmaudio: stdout must be a pipe to a player\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    fcntl(1, F_SETPIPE_SZ, 4096); // a small pipe: sound waiting in it is sound played late
    if (music_path) {
        music = load_mp3(music_path);
        if (!music.frames) fprintf(stderr, "rmaudio: can't read music %s\n", music_path);
    }
    fprintf(stderr, "rmaudio: playing through a pipe\n");

    int16_t buf[CHUNK * 2];
    char line[512];
    size_t len = 0;
    long long start = now_ns();
    long written = 0; // frames
    for (;;) {
        long played = (long)((now_ns() - start) * RATE / 1000000000LL);
        long lead = RATE * LEAD_MS / 1000;
        if (written - played < -lead) written = played; // after a stall: skip, don't burst
        while (written - played < lead) {
            mix(buf);
            if (!write_all(buf, sizeof buf)) {
                fprintf(stderr, "rmaudio: the player stopped (%s)\n", strerror(errno));
                return 1;
            }
            written += CHUNK;
        }
        int wait = (int)((written - played - lead) * 1000 / RATE) + 1;
        struct pollfd p = {ctl, POLLIN, 0};
        if (poll(&p, 1, wait) > 0) {
            ssize_t n = read(ctl, line + len, sizeof line - 1 - len);
            if (n > 0) len += n;
            char *nl;
            while ((nl = memchr(line, '\n', len))) {
                *nl = 0;
                if (!command(line)) return 0;
                len -= nl + 1 - line;
                memmove(line, nl + 1, len);
            }
            if (len == sizeof line - 1) len = 0; // a line too long: drop it
        }
    }
}
