// extern "C" wrapper around libKeyFinder for the bpmkey plugin.
//
// Caller passes mono float32 PCM at any sample rate; we wrap it in a
// KeyFinder::AudioData and run keyOfAudio(), then map key_t to a short
// Camelot-friendly tag like "Am", "F#", "Cm", "C".

#include <keyfinder/keyfinder.h>
#include <cstring>
#include <cstddef>
#include <mutex>

// libfftw3 plan creation is not thread-safe. KeyFinder creates plans per call,
// so multiple worker threads racing into keyOfAudio() segfault inside FFTW.
// Serialize the whole analysis behind a global mutex; decode + BPM stay parallel.
static std::mutex g_kf_mutex;

extern "C" int kf_analyze(const float *mono, size_t frames, int rate,
                          char *out, size_t outlen) {
    if (!mono || frames == 0 || rate <= 0 || !out || outlen < 4) return -1;

    // Reject silent / NaN / inf input — KeyFinder throws on these.
    double sumsq = 0.0;
    int nan_seen = 0;
    for (size_t i = 0; i < frames; ++i) {
        float s = mono[i];
        if (!(s == s) || s > 1e9f || s < -1e9f) { nan_seen = 1; continue; }
        sumsq += static_cast<double>(s) * s;
    }
    if (nan_seen || sumsq < 1e-9) { out[0] = '\0'; return -1; }

    KeyFinder::key_t k;
    try {
        KeyFinder::AudioData a;
        a.setFrameRate(static_cast<unsigned int>(rate));
        a.setChannels(1);
        a.addToSampleCount(static_cast<unsigned int>(frames));
        for (size_t i = 0; i < frames; ++i) {
            a.setSample(static_cast<unsigned int>(i), static_cast<double>(mono[i]));
        }
        std::lock_guard<std::mutex> lock(g_kf_mutex);
        KeyFinder::KeyFinder kf;
        k = kf.keyOfAudio(a);
    } catch (const std::exception &) {
        out[0] = '\0';
        return -1;
    } catch (...) {
        out[0] = '\0';
        return -1;
    }

    static const char *names[25] = {
        "A",  "Am",  "Bb",  "Bbm", "B",   "Bm",
        "C",  "Cm",  "Db",  "Dbm", "D",   "Dm",
        "Eb", "Ebm", "E",   "Em",  "F",   "Fm",
        "Gb", "Gbm", "G",   "Gm",  "Ab",  "Abm",
        ""
    };
    if (k < 0 || k > 24) return -1;
    if (k == 24) { out[0] = '\0'; return -1; } // SILENCE
    std::strncpy(out, names[k], outlen - 1);
    out[outlen - 1] = '\0';
    return 0;
}
