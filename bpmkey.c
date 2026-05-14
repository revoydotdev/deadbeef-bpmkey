/*
 * bpmkey - DeaDBeeF plugin: BPM and musical-key detection.
 *
 * Background-analyzes every track in PL_MAIN, writes "bpm" and "key"
 * metadata, persists to file tags via write_metadata(), and emits
 * DB_EV_TRACKINFOCHANGED so columns refresh live.
 *
 * Add columns in DeaDBeeF: View -> Design Mode -> right-click header ->
 *   Add column ... -> Format = "%bpm%"  / "%key%"
 *
 * Config (deadbeef.conf):
 *   bpmkey.threads = 1     ; worker count, capped at 32
 *   bpmkey.write_tags = 1  ; write to file tags (1) or DB-only (0)
 *   bpmkey.skip_existing = 1
 *
 * Manual rescan: deadbeef --plugin=bpmkey rescan
 */

#define DDB_API_LEVEL 18
#include <deadbeef/deadbeef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#include <aubio/aubio.h>

extern int kf_analyze(const float *mono, size_t frames, int rate,
                      char *out, size_t outlen);

static DB_functions_t *deadbeef;
static DB_misc_t plugin;

#define MAX_WORKERS 32
#define HOP_SIZE    512
#define WIN_SIZE    1024
#define KEY_RATE    11025
#define KEY_MAX_FRAMES (90 * KEY_RATE)   /* cap key analysis at 90s of audio */
#define READ_FRAMES 4096

/* ---------- queue ---------- */
typedef struct qnode_s {
    DB_playItem_t *it;
    int force;
    struct qnode_s *next;
} qnode_t;

static qnode_t *q_head, *q_tail;
static int q_size;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static volatile int stopping;

/* progress reporting */
static pthread_mutex_t prog_mtx = PTHREAD_MUTEX_INITIALIZER;
static int   prog_done;
static time_t prog_start;
static time_t prog_last_log;

static pthread_t workers[MAX_WORKERS];
static int n_workers;

static void q_push(DB_playItem_t *it, int force) {
    qnode_t *n = malloc(sizeof(*n));
    if (!n) return;
    n->it = it;
    n->force = force;
    n->next = NULL;
    deadbeef->pl_item_ref(it);
    pthread_mutex_lock(&q_mtx);
    if (q_tail) q_tail->next = n; else q_head = n;
    q_tail = n;
    q_size++;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mtx);
}

static qnode_t *q_pop_block(void) {
    pthread_mutex_lock(&q_mtx);
    while (!q_head && !stopping) pthread_cond_wait(&q_cv, &q_mtx);
    qnode_t *n = q_head;
    if (n) {
        q_head = n->next;
        if (!q_head) q_tail = NULL;
        q_size--;
    }
    pthread_mutex_unlock(&q_mtx);
    return n;
}

static void q_drain(void) {
    pthread_mutex_lock(&q_mtx);
    qnode_t *n = q_head; q_head = q_tail = NULL; q_size = 0;
    pthread_mutex_unlock(&q_mtx);
    while (n) {
        qnode_t *nx = n->next;
        deadbeef->pl_item_unref(n->it);
        free(n);
        n = nx;
    }
}

/* ---------- decoder lookup ---------- */
static DB_decoder_t *find_decoder(DB_playItem_t *it) {
    char dec_id[128] = {0};
    deadbeef->pl_lock();
    const char *raw = deadbeef->pl_find_meta_raw(it, ":DECODER");
    if (raw) { strncpy(dec_id, raw, sizeof(dec_id)-1); }
    deadbeef->pl_unlock();
    if (!dec_id[0]) return NULL;
    DB_decoder_t **list = deadbeef->plug_get_decoder_list();
    for (int i = 0; list && list[i]; i++) {
        if (strcmp(list[i]->plugin.id, dec_id) == 0) return list[i];
    }
    return NULL;
}

/* ---------- analyze one track ---------- */
typedef struct {
    float    *key_buf;     /* mono float32 @ KEY_RATE */
    size_t    key_cap;
    size_t    key_len;
    int       key_step;    /* native_rate / KEY_RATE */
    int       key_phase;
    aubio_tempo_t *tempo;
    fvec_t   *tempo_in;    /* HOP_SIZE */
    fvec_t   *tempo_out;
    size_t    tempo_fill;
} analyzer_t;

static void feed_mono(analyzer_t *a, const float *mono, size_t frames) {
    /* aubio: hop-aligned input */
    for (size_t i = 0; i < frames; i++) {
        a->tempo_in->data[a->tempo_fill++] = mono[i];
        if (a->tempo_fill == HOP_SIZE) {
            aubio_tempo_do(a->tempo, a->tempo_in, a->tempo_out);
            a->tempo_fill = 0;
        }
        /* key: simple decimation, capped at KEY_MAX_FRAMES */
        if (a->key_len < KEY_MAX_FRAMES && ++a->key_phase >= a->key_step) {
            a->key_phase = 0;
            float v = mono[i];
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

static int analyze_track(DB_playItem_t *it, int force, int write_tags) {
    if (!force) {
        deadbeef->pl_lock();
        const char *bpm = deadbeef->pl_find_meta(it, "bpm");
        const char *key = deadbeef->pl_find_meta(it, "key");
        int have_both = (bpm && *bpm) && (key && *key);
        deadbeef->pl_unlock();
        if (have_both) return 0;
    }

    DB_decoder_t *dec = find_decoder(it);
    if (!dec) {
        deadbeef->log_detailed(&plugin.plugin, 0, "bpmkey: no decoder for track\n");
        return -1;
    }

    DB_fileinfo_t *fi = NULL;
    if (dec->open2) fi = dec->open2(DDB_DECODER_HINT_RAW_SIGNAL, it);
    if (!fi)        fi = dec->open(DDB_DECODER_HINT_RAW_SIGNAL);
    if (!fi) return -1;
    if (dec->init(fi, it) < 0) { dec->free(fi); return -1; }

    int sr = fi->fmt.samplerate;
    int ch = fi->fmt.channels;
    int bps = fi->fmt.bps;
    if (sr <= 0 || ch <= 0 || bps <= 0) { dec->free(fi); return -1; }

    /* Conversion buffers: read raw -> float32 (same SR/ch) -> mix to mono */
    ddb_waveformat_t outfmt = fi->fmt;
    outfmt.bps = 32; outfmt.is_float = 1;
    int frame_in_bytes  = (bps / 8) * ch;
    int frame_out_bytes = 4 * ch;
    int read_bytes = READ_FRAMES * frame_in_bytes;

    char  *raw  = malloc(read_bytes);
    float *fbuf = malloc(READ_FRAMES * frame_out_bytes);
    float *mono = malloc(READ_FRAMES * sizeof(float));
    if (!raw || !fbuf || !mono) { free(raw); free(fbuf); free(mono); dec->free(fi); return -1; }

    analyzer_t A = {0};
    A.tempo = new_aubio_tempo("default", WIN_SIZE, HOP_SIZE, sr);
    A.tempo_in = new_fvec(HOP_SIZE);
    A.tempo_out = new_fvec(2);
    A.key_step = sr / KEY_RATE; if (A.key_step < 1) A.key_step = 1;

    if (!A.tempo || !A.tempo_in || !A.tempo_out) {
        if (A.tempo) del_aubio_tempo(A.tempo);
        if (A.tempo_in) del_fvec(A.tempo_in);
        if (A.tempo_out) del_fvec(A.tempo_out);
        free(raw); free(fbuf); free(mono); dec->free(fi); return -1;
    }

    int total_frames = 0;
    for (;;) {
        if (stopping) break;
        int got = dec->read(fi, raw, read_bytes);
        if (got <= 0) break;
        int frames = got / frame_in_bytes;
        if (frames <= 0) break;

        /* Convert to interleaved float32 */
        if (deadbeef->pcm_convert(&fi->fmt, raw, &outfmt, (char*)fbuf, frames * frame_in_bytes) <= 0) break;

        /* Mix to mono */
        for (int i = 0; i < frames; i++) {
            float s = 0.f;
            for (int c = 0; c < ch; c++) s += fbuf[i*ch + c];
            mono[i] = s / (float)ch;
        }

        feed_mono(&A, mono, frames);
        total_frames += frames;
    }

    /* finalize */
    float bpm_val = aubio_tempo_get_bpm(A.tempo);
    float bpm_conf = aubio_tempo_get_confidence(A.tempo);
    char keystr[16] = {0};
    int key_ok = -1;
    if (A.key_len > KEY_RATE) {  /* need at least ~1s */
        key_ok = kf_analyze(A.key_buf, A.key_len, KEY_RATE, keystr, sizeof(keystr));
    }

    /* cleanup analyzer */
    del_aubio_tempo(A.tempo);
    del_fvec(A.tempo_in);
    del_fvec(A.tempo_out);
    free(A.key_buf);
    free(raw); free(fbuf); free(mono);
    dec->free(fi);

    if (stopping) return 0;
    if (total_frames < sr) {  /* < 1s decoded */
        deadbeef->log_detailed(&plugin.plugin, 0, "bpmkey: track too short, skipped\n");
        return -1;
    }

    char bpm_buf[32];
    if (bpm_val > 1.0f && bpm_val < 1000.0f && bpm_conf > 0.0f && (bpm_val == bpm_val)) {
        snprintf(bpm_buf, sizeof(bpm_buf), "%.1f", bpm_val);
    } else {
        bpm_buf[0] = '\0';
    }

    deadbeef->pl_lock();
    if (bpm_buf[0]) deadbeef->pl_replace_meta(it, "bpm", bpm_buf);
    if (key_ok == 0 && keystr[0]) deadbeef->pl_replace_meta(it, "key", keystr);
    deadbeef->pl_unlock();

    if (write_tags && (bpm_buf[0] || (key_ok == 0 && keystr[0]))) {
        if (dec->write_metadata) dec->write_metadata(it);
    }

    ddb_event_track_t *ev = (ddb_event_track_t *)deadbeef->event_alloc(DB_EV_TRACKINFOCHANGED);
    ev->track = it; deadbeef->pl_item_ref(it);
    deadbeef->event_send((ddb_event_t *)ev, 0, 0);

    deadbeef->log_detailed(&plugin.plugin, 0,
        "bpmkey: bpm=%s key=%s\n",
        bpm_buf[0] ? bpm_buf : "?",
        (key_ok == 0 && keystr[0]) ? keystr : "?");
    return 0;
}

/* ---------- worker ---------- */
static void *worker_main(void *arg) {
    (void)arg;
    int write_tags = deadbeef->conf_get_int("bpmkey.write_tags", 1);
    while (!stopping) {
        qnode_t *n = q_pop_block();
        if (!n) break;
        analyze_track(n->it, n->force, write_tags);
        deadbeef->pl_item_unref(n->it);
        free(n);

        pthread_mutex_lock(&prog_mtx);
        prog_done++;
        time_t now = time(NULL);
        if (now - prog_last_log >= 10 || (prog_done % 100 == 0)) {
            int elapsed = (int)(now - prog_start);
            double tps = elapsed > 0 ? (double)prog_done / elapsed : 0.0;
            int remaining = q_size;
            int eta = (tps > 0.01 && remaining > 0) ? (int)(remaining / tps) : -1;
            prog_last_log = now;
            pthread_mutex_unlock(&prog_mtx);
            deadbeef->log_detailed(&plugin.plugin, 0,
                "bpmkey: progress %d done, %d in queue, %.2f tracks/s, elapsed %ds, eta %ds\n",
                prog_done, remaining, tps, elapsed, eta);
        } else {
            pthread_mutex_unlock(&prog_mtx);
        }
    }
    return NULL;
}

/* ---------- enqueue helpers ---------- */
/* simple open-addressing string set for URI dedup, linear probing */
typedef struct { char **keys; size_t cap; size_t len; } strset_t;
static unsigned long djb2(const char *s) {
    unsigned long h = 5381; int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}
static int strset_add(strset_t *s, const char *k) {
    if (s->len * 2 >= s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        char **nk = calloc(nc, sizeof(char*));
        if (!nk) return -1;
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->keys[i]) continue;
            unsigned long h = djb2(s->keys[i]) & (nc-1);
            while (nk[h]) h = (h+1) & (nc-1);
            nk[h] = s->keys[i];
        }
        free(s->keys); s->keys = nk; s->cap = nc;
    }
    unsigned long h = djb2(k) & (s->cap-1);
    while (s->keys[h]) {
        if (strcmp(s->keys[h], k) == 0) return 1;  /* dup */
        h = (h+1) & (s->cap-1);
    }
    s->keys[h] = strdup(k);
    s->len++;
    return 0;
}
static void strset_free(strset_t *s) {
    if (!s->keys) return;
    for (size_t i = 0; i < s->cap; i++) free(s->keys[i]);
    free(s->keys);
    s->keys = NULL; s->cap = s->len = 0;
}

static void enqueue_all(int force) {
    int skip_existing = deadbeef->conf_get_int("bpmkey.skip_existing", 1);
    int n = 0, seen = 0, dups = 0;
    int npl = deadbeef->plt_get_count();
    strset_t seen_uris = {0};
    for (int p = 0; p < npl; p++) {
        deadbeef->pl_lock();
        ddb_playlist_t *plt = deadbeef->plt_get_for_idx(p);
        if (!plt) { deadbeef->pl_unlock(); continue; }
        DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
        while (it) {
            seen++;
            const char *uri = deadbeef->pl_find_meta(it, ":URI");
            int dup = 0;
            if (uri) dup = (strset_add(&seen_uris, uri) == 1);
            if (!dup) {
                int wanted = force || !skip_existing;
                if (!wanted) {
                    const char *bpm = deadbeef->pl_find_meta(it, "bpm");
                    const char *key = deadbeef->pl_find_meta(it, "key");
                    wanted = !((bpm && *bpm) && (key && *key));
                }
                if (wanted) { q_push(it, force); n++; }
            } else {
                dups++;
            }
            DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
            deadbeef->pl_item_unref(it);
            it = next;
        }
        deadbeef->plt_unref(plt);
        deadbeef->pl_unlock();
    }
    strset_free(&seen_uris);
    deadbeef->log_detailed(&plugin.plugin, 0,
        "bpmkey: queued %d/%d tracks (%d dup-skipped) across %d playlist(s) (force=%d, workers=%d)\n",
        n, seen, dups, npl, force, n_workers);
}

/* ---------- plugin entry points ---------- */
static int bpmkey_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2);
static int bpmkey_connect(void);
static int bpmkey_disconnect(void);
static int bpmkey_exec_cmdline(const char *cmdline, int cmdline_size, ddb_response_t *response);

static int bpmkey_connect(void) {
    int t = deadbeef->conf_get_int("bpmkey.threads", 1);
    if (t < 1) t = 1;
    if (t > MAX_WORKERS) t = MAX_WORKERS;
    n_workers = t;
    stopping = 0;
    for (int i = 0; i < n_workers; i++) {
        if (pthread_create(&workers[i], NULL, worker_main, NULL) != 0) {
            n_workers = i; break;
        }
    }
    deadbeef->log_detailed(&plugin.plugin, 0, "bpmkey: connected, %d worker(s)\n", n_workers);
    return 0;
}

static int bpmkey_disconnect(void) {
    pthread_mutex_lock(&q_mtx);
    stopping = 1;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mtx);
    for (int i = 0; i < n_workers; i++) pthread_join(workers[i], NULL);
    n_workers = 0;
    q_drain();
    return 0;
}

static int bpmkey_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    (void)ctx; (void)p1; (void)p2;
    if (id == DB_EV_PLUGINSLOADED) {
        if (deadbeef->conf_get_int("bpmkey.scan_on_start", 1)) enqueue_all(0);
    } else if (id == DB_EV_PLAYLISTCHANGED) {
        /* Light reactive scan: enqueue any new tracks missing tags. */
        if (deadbeef->conf_get_int("bpmkey.reactive", 1)) enqueue_all(0);
    }
    return 0;
}

static int bpmkey_exec_cmdline(const char *cmdline, int cmdline_size, ddb_response_t *response) {
    int force = 0, do_scan = 0, do_status = 0;
    const char *scandir = NULL;
    const char *p = cmdline;
    while (p && p < cmdline + cmdline_size) {
        if (*p) {
            if (strcmp(p, "rescan") == 0 || strcmp(p, "force") == 0) { force = 1; do_scan = 1; }
            else if (strcmp(p, "scan") == 0) do_scan = 1;
            else if (strcmp(p, "status") == 0) do_status = 1;
            else if (strcmp(p, "scandir") == 0) {
                p += strlen(p) + 1;
                if (p < cmdline + cmdline_size && *p) scandir = p;
                do_scan = 1; force = 1;
                continue;
            }
        }
        p += strlen(p) + 1;
    }
    if (!do_scan && !do_status) do_status = 1;  /* default: don't mutate */

    if (scandir) {
        int idx = deadbeef->plt_add(deadbeef->plt_get_count(), "bpmkey-scan");
        ddb_playlist_t *plt = deadbeef->plt_get_for_idx(idx);
        if (plt) {
            int abort_flag = 0;
            deadbeef->plt_insert_dir2(0, plt, NULL, scandir, &abort_flag, NULL, NULL);
            deadbeef->plt_unref(plt);
        }
    }

    if (do_scan) {
        pthread_mutex_lock(&prog_mtx);
        prog_done = 0;
        prog_start = time(NULL);
        prog_last_log = 0;
        pthread_mutex_unlock(&prog_mtx);
        enqueue_all(force);
    }

    if (response) {
        char msg[320];
        pthread_mutex_lock(&prog_mtx);
        int done = prog_done;
        int elapsed = prog_start ? (int)(time(NULL) - prog_start) : 0;
        pthread_mutex_unlock(&prog_mtx);
        double tps = elapsed > 0 ? (double)done / elapsed : 0.0;
        int n = snprintf(msg, sizeof(msg),
            "bpmkey: queue=%d done=%d elapsed=%ds tps=%.2f workers=%d%s\n",
            q_size, done, elapsed, tps, n_workers,
            do_scan ? (force ? " (force scan triggered)" : " (incremental scan triggered)") : "");
        response->append(response, msg, n);
    }
    return 0;
}

static const char bpmkey_settings_dlg[] =
    "property \"Worker threads\" entry bpmkey.threads 1;\n"
    "property \"Write to file tags\" checkbox bpmkey.write_tags 1;\n"
    "property \"Skip already-tagged tracks\" checkbox bpmkey.skip_existing 1;\n"
    "property \"Scan whole library on startup\" checkbox bpmkey.scan_on_start 1;\n"
    "property \"React to playlist changes\" checkbox bpmkey.reactive 1;\n";

static DB_misc_t plugin = {
    .plugin = {
        .api_vmajor = DB_API_VERSION_MAJOR,
        .api_vminor = DB_API_VERSION_MINOR,
        .type = DB_PLUGIN_MISC,
        .version_major = 1,
        .version_minor = 0,
        .id = "bpmkey",
        .name = "BPM and Key Detector",
        .descr = "Background BPM/key analysis. Adds 'bpm' and 'key' track metadata.\n"
                 "Use %bpm% and %key% in title formatting / column format.",
        .copyright = "GPL-3.0-or-later",
        .connect = bpmkey_connect,
        .disconnect = bpmkey_disconnect,
        .exec_cmdline = bpmkey_exec_cmdline,
        .message = bpmkey_message,
        .configdialog = bpmkey_settings_dlg,
    },
};

DB_plugin_t *bpmkey_load(DB_functions_t *api) {
    deadbeef = api;
    return &plugin.plugin;
}
