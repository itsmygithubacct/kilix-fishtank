/* Optional ambience plus edge-triggered game events through the PCM mixer. */
#include "fishtank.h"
#include "pcm_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int16_t *data;
    size_t frames;
} AudioSample;

static const char *const event_files[AUDIO_EVENT_COUNT] = {
    [AUDIO_FEED_SPLASH] = "events/feed_splash.wav",
    [AUDIO_FISH_BITE] = "events/fish_bite.wav",
    [AUDIO_SHARK_BITE] = "events/shark_bite.wav",
    [AUDIO_FRENZY] = "events/frenzy.wav",
    [AUDIO_CATCH_SPLASH] = "events/catch_splash.wav",
};

static pcmmix mixer;
static bool mixer_started;
static AudioSample ambience;
static AudioSample events[AUDIO_EVENT_COUNT];
static char audio_root[1400] = "assets/audio";

static bool env_is_off(const char *value)
{
    return value && (!strcmp(value, "0") || !strcmp(value, "off") ||
                     !strcmp(value, "false") || !strcmp(value, "none"));
}

static bool audio_disabled(void)
{
    return getenv("KILIX_FISHTANK_NO_AUDIO") ||
           env_is_off(getenv("KILIX_FISHTANK_AUDIO"));
}

static bool readable_file(const char *path)
{
    return path && *path && access(path, R_OK) == 0;
}

static bool readable_directory(const char *path)
{
    return path && *path && access(path, R_OK) == 0;
}

static bool executable_dir(const char *argv0, char *out, size_t out_size)
{
    char *slash;
    if (!argv0 || !strchr(argv0, '/')) return false;
    (void)snprintf(out, out_size, "%s", argv0);
    slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static void find_audio_root(const char *argv0)
{
    const char *override = getenv("KILIX_FISHTANK_ASSETS");
    char directory[1024];
    char candidate[1400];

    if (override && *override) {
        (void)snprintf(audio_root, sizeof audio_root, "%s/audio", override);
        return;
    }
    if (readable_directory("assets/audio")) return;
    if (!executable_dir(argv0, directory, sizeof directory)) return;
    (void)snprintf(candidate, sizeof candidate, "%s/assets/audio", directory);
    if (readable_directory(candidate)) {
        (void)snprintf(audio_root, sizeof audio_root, "%s", candidate);
        return;
    }
    (void)snprintf(candidate, sizeof candidate,
                   "%s/../share/kilix-fishtank/assets/audio", directory);
    if (readable_directory(candidate))
        (void)snprintf(audio_root, sizeof audio_root, "%s", candidate);
}

static bool load_file(const char *path, AudioSample *sample, bool report)
{
    char error[256];
    sample->data = pcmmix_wav_load(path, &sample->frames, error, sizeof error);
    if (sample->data) return true;
    if (report)
        (void)fprintf(stderr, "invalid or missing sound: %s (%s)\n", path,
                      error);
    return false;
}

static bool load_bank(const char *argv0, bool report)
{
    const char *explicit_path = getenv("KILIX_FISHTANK_AUDIO");
    char path[1800];
    bool loaded = true;

    find_audio_root(argv0);
    if (explicit_path && *explicit_path && !env_is_off(explicit_path) &&
        strstr(explicit_path, ".wav"))
        (void)snprintf(path, sizeof path, "%s", explicit_path);
    else
        (void)snprintf(path, sizeof path, "%s/fishtank_ambience.wav",
                       audio_root);
    if (!readable_file(path) || !load_file(path, &ambience, report))
        loaded = false;
    for (int id = 0; id < AUDIO_EVENT_COUNT; ++id) {
        (void)snprintf(path, sizeof path, "%s/%s", audio_root,
                       event_files[id]);
        if (!load_file(path, &events[id], report)) loaded = false;
    }
    return loaded;
}

static void free_bank(void)
{
    pcmmix_wav_free(ambience.data);
    ambience = (AudioSample){0};
    for (int id = 0; id < AUDIO_EVENT_COUNT; ++id) {
        pcmmix_wav_free(events[id].data);
        events[id] = (AudioSample){0};
    }
}

static float audio_volume(void)
{
    const char *value = getenv("KILIX_FISHTANK_AUDIO_VOLUME");
    int volume = value && *value ? atoi(value) : 18;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume / 100.0f;
}

void audio_start(const char *argv0)
{
    pcmmix_options options;
    pcmmix_sample clip;

    if (mixer_started || audio_disabled()) return;
    (void)load_bank(argv0, false);
    if (!ambience.data) {
        free_bank();
        return;
    }
    pcmmix_options_init(&options);
    options.max_voices = 24;
    if (!pcmmix_start(&mixer, &options)) {
        free_bank();
        return;
    }
    mixer_started = true;
    clip = (pcmmix_sample){ambience.data, ambience.frames};
    (void)pcmmix_loop(&mixer, &clip, audio_volume(), 1.0f);
}

void audio_play(audio_event event, float volume, float pitch)
{
    pcmmix_sample clip;
    if (!mixer_started || event < 0 || event >= AUDIO_EVENT_COUNT ||
        !events[event].data)
        return;
    clip = (pcmmix_sample){events[event].data, events[event].frames};
    (void)pcmmix_play(&mixer, &clip, volume * audio_volume(), pitch);
}

void audio_stop(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    free_bank();
}

void audio_emergency_stop(void)
{
    /* The fatal-signal caller exits immediately; the sink observes EOF. */
}

void audio_toggle(const char *argv0)
{
    if (mixer_started) audio_stop();
    else audio_start(argv0);
}

bool audio_validate_assets(const char *argv0)
{
    bool valid;
    free_bank();
    valid = load_bank(argv0, true);
    free_bank();
    return valid;
}
