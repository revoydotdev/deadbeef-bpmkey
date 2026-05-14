/*
 * bpmkey-test: standalone CLI tester replicating the plugin's analysis chain.
 *
 * Decodes via ffmpeg subprocess (mono f32le @ 44100 Hz), runs identical
 * aubio_tempo + libKeyFinder analysis as bpmkey.c.
 *
 * Usage: bpmkey-test <file1> [file2 ...]
 * Output: TSV: filepath<TAB>bpm<TAB>key<TAB>confidence
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aubio/aubio.h>

extern int kf_analyze(const float *mono, size_t frames, int rate,
                      char *out, size_t outlen);

#define HOP_SIZE   512
#define WIN_SIZE   1024
#define SR         44100
#define KEY_RATE   11025
#define KEY_MAX_FRAMES (90 * KEY_RATE)
#define READ_FRAMES 4096

typedef struct {
    float *key_buf; size_t key_cap, key_len;
    int key_step, key_phase;
    aubio_tempo_t *tempo;
    fvec_t *tin, *tout;
    size_t fill;
} analyzer_t;

static void feed(analyzer_t *a, const float *m, size_t n) {
    for (size_t i = 0; i < n; i++) {
        a->tin->data[a->fill++] = m[i];
        if (a->fill == HOP_SIZE) {
            aubio_tempo_do(a->tempo, a->tin, a->tout);
            a->fill = 0;
        }
        if (a->key_len < KEY_MAX_FRAMES && ++a->key_phase >= a->key_step) {
            a->key_phase = 0;
            float v = m[i];
            if (!(v == v) || v > 1e6f || v < -1e6f) v = 0.f;
            if (a->key_len >= a->key_cap) {
                size_t nc = a->key_cap ? a->key_cap * 2 : 65536;
                if (nc > KEY_MAX_FRAMES) nc = KEY_MAX_FRAMES;
                float *nb = realloc(a->key_buf, nc * sizeof(float));
                if (!nb) return;
                a->key_buf = nb; a->key_cap = nc;
            }
            a->key_buf[a->key_len++] = v;
        }
    }
}

static int analyze_file(const char *path) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -nostdin -v error -i \"%s\" -f f32le -ac 1 -ar %d - 2>/dev/null",
        path, SR);
    FILE *fp = popen(cmd, "r");
    if (!fp) { fprintf(stderr, "popen failed: %s\n", path); return -1; }

    analyzer_t A = {0};
    A.tempo = new_aubio_tempo("default", WIN_SIZE, HOP_SIZE, SR);
    A.tin = new_fvec(HOP_SIZE);
    A.tout = new_fvec(2);
    A.key_step = SR / KEY_RATE; if (A.key_step < 1) A.key_step = 1;

    float buf[READ_FRAMES];
    size_t n; size_t total = 0;
    while ((n = fread(buf, sizeof(float), READ_FRAMES, fp)) > 0) {
        feed(&A, buf, n);
        total += n;
    }
    pclose(fp);

    float bpm = aubio_tempo_get_bpm(A.tempo);
    float conf = aubio_tempo_get_confidence(A.tempo);
    char keystr[16] = {0};
    int key_ok = -1;
    if (A.key_len > KEY_RATE)
        key_ok = kf_analyze(A.key_buf, A.key_len, KEY_RATE, keystr, sizeof(keystr));

    del_aubio_tempo(A.tempo);
    del_fvec(A.tin); del_fvec(A.tout);
    free(A.key_buf);

    if (total < SR) {
        printf("%s\t-\t-\t-\n", path);
        return -1;
    }
    char bpmstr[32];
    if (bpm > 1.0f && bpm < 1000.0f && conf > 0.0f && bpm == bpm)
        snprintf(bpmstr, sizeof(bpmstr), "%.1f", bpm);
    else
        snprintf(bpmstr, sizeof(bpmstr), "-");

    printf("%s\t%s\t%s\t%.3f\n", path, bpmstr,
        (key_ok == 0 && keystr[0]) ? keystr : "-", conf);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file> [file...]\n", argv[0]);
        return 1;
    }
    printf("path\tbpm\tkey\tbpm_confidence\n");
    for (int i = 1; i < argc; i++) analyze_file(argv[i]);
    return 0;
}
