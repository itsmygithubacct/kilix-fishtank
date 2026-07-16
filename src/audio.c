/* Optional looping ambience through the shared PCM mixer. */
#include "fishtank.h"
#include "pcm_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pcmmix mixer;
static bool mixer_started;
static int16_t *ambience_data;
static size_t ambience_frames;

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

static bool try_path(char *out, size_t out_size, const char *path)
{
    if (!readable_file(path)) return false;
    snprintf(out, out_size, "%s", path);
    return true;
}

static bool executable_dir(const char *argv0, char *out, size_t out_size)
{
    if (!argv0 || !strchr(argv0, '/')) return false;
    snprintf(out, out_size, "%s", argv0);
    char *slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static bool find_audio_asset(const char *argv0, char *out, size_t out_size)
{
    const char *explicit_path = getenv("KILIX_FISHTANK_AUDIO");
    if (explicit_path && *explicit_path && !env_is_off(explicit_path) &&
        strstr(explicit_path, ".wav"))
        return try_path(out, out_size, explicit_path);
    if (try_path(out, out_size, "assets/audio/fishtank_ambience.wav"))
        return true;

    char directory[1024];
    char candidate[1400];
    if (executable_dir(argv0, directory, sizeof directory)) {
        snprintf(candidate, sizeof candidate,
                 "%s/assets/audio/fishtank_ambience.wav", directory);
        if (try_path(out, out_size, candidate)) return true;
        snprintf(candidate, sizeof candidate,
                 "%s/../assets/audio/fishtank_ambience.wav", directory);
        if (try_path(out, out_size, candidate)) return true;
    }
    return false;
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
    char asset[1400];
    char error[256];

    if (mixer_started || audio_disabled() ||
        !find_audio_asset(argv0, asset, sizeof asset))
        return;
    ambience_data = pcmmix_wav_load(asset, &ambience_frames,
                                     error, sizeof error);
    if (!ambience_data) return;
    pcmmix_options_init(&options);
    if (!pcmmix_start(&mixer, &options)) {
        pcmmix_wav_free(ambience_data);
        ambience_data = NULL;
        ambience_frames = 0;
        return;
    }
    mixer_started = true;
    pcmmix_sample ambience = {ambience_data, ambience_frames};
    (void)pcmmix_loop(&mixer, &ambience, audio_volume(), 1.0f);
}

void audio_stop(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    pcmmix_wav_free(ambience_data);
    ambience_data = NULL;
    ambience_frames = 0;
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
