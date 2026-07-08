/* Optional ambience playback through ffplay. */
#include "fishtank.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t audioPid = -1;

static bool env_is_off(const char *value)
{
    return value && (!strcmp(value, "0") || !strcmp(value, "off") ||
                     !strcmp(value, "false") || !strcmp(value, "none"));
}

static bool audio_disabled(void)
{
    return getenv("KILIX_FISHTANK_NO_AUDIO") || env_is_off(getenv("KILIX_FISHTANK_AUDIO"));
}

static bool readable_file(const char *path)
{
    return path && *path && access(path, R_OK) == 0;
}

static bool try_path(char *out, size_t outSize, const char *path)
{
    if (!readable_file(path)) return false;
    snprintf(out, outSize, "%s", path);
    return true;
}

static bool executable_dir(const char *argv0, char *out, size_t outSize)
{
    if (!argv0 || !strchr(argv0, '/')) return false;
    snprintf(out, outSize, "%s", argv0);
    char *slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static bool find_audio_asset(const char *argv0, char *out, size_t outSize)
{
    const char *explicitPath = getenv("KILIX_FISHTANK_AUDIO");
    if (explicitPath && *explicitPath && !env_is_off(explicitPath))
        return try_path(out, outSize, explicitPath);

    if (try_path(out, outSize, "assets/audio/fishtank_ambience.webm")) return true;
    if (try_path(out, outSize, "assets/audio/fishtank_ambience.wav")) return true;

    char dir[1024];
    char candidate[1400];
    if (executable_dir(argv0, dir, sizeof dir)) {
        snprintf(candidate, sizeof candidate, "%s/assets/audio/fishtank_ambience.webm", dir);
        if (try_path(out, outSize, candidate)) return true;
        snprintf(candidate, sizeof candidate, "%s/assets/audio/fishtank_ambience.wav", dir);
        if (try_path(out, outSize, candidate)) return true;
        snprintf(candidate, sizeof candidate, "%s/../assets/audio/fishtank_ambience.webm", dir);
        if (try_path(out, outSize, candidate)) return true;
        snprintf(candidate, sizeof candidate, "%s/../assets/audio/fishtank_ambience.wav", dir);
        if (try_path(out, outSize, candidate)) return true;
    }

    return false;
}

static void reap_audio(void)
{
    if (audioPid <= 0) return;
    int status;
    pid_t got = waitpid(audioPid, &status, WNOHANG);
    if (got == audioPid) audioPid = -1;
}

static int audio_volume(void)
{
    const char *env = getenv("KILIX_FISHTANK_AUDIO_VOLUME");
    if (!env || !*env) return 18;
    int volume = atoi(env);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume;
}

void audio_start(const char *argv0)
{
    reap_audio();
    if (audioPid > 0 || audio_disabled()) return;

    char asset[1400];
    if (!find_audio_asset(argv0, asset, sizeof asset)) return;

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        char volumeArg[16];
        snprintf(volumeArg, sizeof volumeArg, "%d", audio_volume());

        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);

        execlp("ffplay", "ffplay",
               "-hide_banner", "-nostats", "-loglevel", "quiet",
               "-nodisp", "-autoexit",
               "-volume", volumeArg,
               "-loop", "0",
               asset,
               (char *)NULL);
        _exit(127);
    }

    audioPid = pid;
}

void audio_stop(void)
{
    reap_audio();
    if (audioPid <= 0) return;
    pid_t pid = audioPid;
    audioPid = -1;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int status;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid || got < 0) return;
        usleep(25000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void audio_emergency_stop(void)
{
    if (audioPid > 0) kill(audioPid, SIGTERM);
}

void audio_toggle(const char *argv0)
{
    reap_audio();
    if (audioPid > 0) audio_stop();
    else audio_start(argv0);
}
