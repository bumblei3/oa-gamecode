// replay_recorder.c — NeonArena Input Recording for Bug Reports
// Records player inputs (move/aim/fire) to binary file for replay/debugging.
// Minimal C implementation for ioq3e dedicated server.

#include "g_local.h"

#ifdef NEONARENA_MOD

#define REPLAY_MAGIC 0x4E525059
#define REPLAY_VERSION 1
#define REPLAY_MAX_EVENTS 32768

struct replayHeader {
    unsigned int magic;
    unsigned short version;
    unsigned short reserved;
    unsigned int eventCount;
    unsigned int durationMs;
    char mapName[64];
};

// replayEvent is defined in g_local.h

static struct replayEvent replayEvents[REPLAY_MAX_EVENTS];
static int replayCount = 0;
static qboolean replayRecording = qfalse;
static qboolean replayPlaying = qfalse;
static int replayPlaybackIdx = 0;
static unsigned int replayStartTime = 0;

// Start recording
void G_ReplayStart(void) {
    if (replayRecording) return;
    replayRecording = qtrue;
    replayPlaying = qfalse;
    replayCount = 0;
    replayStartTime = trap_Milliseconds();
    G_Printf("NeonWave: Replay recording started\n");
}

// Stop recording
void G_ReplayStop(void) {
    if (!replayRecording) return;
    replayRecording = qfalse;
    G_Printf("NeonWave: Replay recording stopped (%d events, %d ms)\n",
             replayCount, replayCount > 0 ? replayEvents[replayCount-1].timestampMs : 0);
}

// Record an event
void G_ReplayRecord(int type, float x, float y, unsigned char buttons) {
    if (!replayRecording || replayCount >= REPLAY_MAX_EVENTS) return;

    struct replayEvent *e = &replayEvents[replayCount];
    e->timestampMs = trap_Milliseconds() - replayStartTime;
    e->type = (unsigned char)type;
    e->x = x;
    e->y = y;
    e->buttons = buttons;
    replayCount++;
}

// Check if recording
qboolean G_ReplayIsRecording(void) {
    return replayRecording;
}

// Get replay count
int G_ReplayGetCount(void) {
    return replayCount;
}

// Save replay to file
void G_ReplaySave(const char *filename) {
    fileHandle_t f;
    char mapNameBuf[MAX_CVAR_VALUE_STRING];
    if (replayCount == 0) {
        G_Printf("NeonWave: Replay save failed — no events recorded\n");
        return;
    }

    if (trap_FS_FOpenFile(filename, &f, FS_WRITE) <= 0) {
        G_Printf("NeonWave: Replay save failed — cannot open %s\n", filename);
        return;
    }

    struct replayHeader header;
    header.magic = REPLAY_MAGIC;
    header.version = REPLAY_VERSION;
    header.reserved = 0;
    header.eventCount = replayCount;
    header.durationMs = replayEvents[replayCount-1].timestampMs;
    trap_Cvar_VariableStringBuffer("mapname", mapNameBuf, sizeof(mapNameBuf));
    Q_strncpyz(header.mapName, mapNameBuf, sizeof(header.mapName));

    trap_FS_Write(&header, sizeof(header), f);
    trap_FS_Write(replayEvents, sizeof(struct replayEvent) * replayCount, f);
    trap_FS_FCloseFile(f);

    G_Printf("NeonWave: Replay saved to %s (%d events)\n", filename, replayCount);
}

// Load replay from file
void G_ReplayLoad(const char *filename) {
    fileHandle_t f;
    struct replayHeader header;

    if (trap_FS_FOpenFile(filename, &f, FS_READ) <= 0) {
        G_Printf("NeonWave: Replay load failed — cannot open %s\n", filename);
        return;
    }

    trap_FS_Read(&header, sizeof(header), f);
    if (header.magic != REPLAY_MAGIC || header.version != REPLAY_VERSION) {
        G_Printf("NeonWave: Replay load failed — bad magic/version\n");
        trap_FS_FCloseFile(f);
        return;
    }

    replayCount = header.eventCount;
    if (replayCount > REPLAY_MAX_EVENTS) replayCount = REPLAY_MAX_EVENTS;
    trap_FS_Read(replayEvents, sizeof(struct replayEvent) * replayCount, f);
    trap_FS_FCloseFile(f);

    G_Printf("NeonWave: Replay loaded from %s (%d events)\n", filename, replayCount);
}

// Start playback
void G_ReplayPlayStart(void) {
    if (replayCount == 0) return;
    replayPlaying = qtrue;
    replayRecording = qfalse;
    replayPlaybackIdx = 0;
    G_Printf("NeonWave: Replay playback started\n");
}

// Stop playback
void G_ReplayPlayStop(void) {
    replayPlaying = qfalse;
}

// Check if playing
qboolean G_ReplayIsPlaying(void) {
    return replayPlaying;
}

// Get next event during playback
// Returns qtrue if event was retrieved
qboolean G_ReplayGetNext(struct replayEvent *out) {
    if (!replayPlaying || replayPlaybackIdx >= replayCount) {
        replayPlaying = qfalse;
        return qfalse;
    }
    *out = replayEvents[replayPlaybackIdx];
    replayPlaybackIdx++;
    if (replayPlaybackIdx >= replayCount) {
        replayPlaying = qfalse;
    }
    return qtrue;
}

// Reset playback to start
void G_ReplayReset(void) {
    replayPlaybackIdx = 0;
}

// Console command: replay control
void G_ReplayCmd_f(void) {
    if (trap_Argc() < 2) {
        G_Printf("Usage: nw_replay <start|stop|save|load|play|status>\n");
        return;
    }
    char cmd[32];
    trap_Argv(1, cmd, sizeof(cmd));

    if (Q_stricmp(cmd, "start") == 0) {
        G_ReplayStart();
    } else if (Q_stricmp(cmd, "stop") == 0) {
        G_ReplayStop();
    } else if (Q_stricmp(cmd, "save") == 0) {
        char filename[64] = "neonwave_replay.dat";
        if (trap_Argc() >= 3) {
            trap_Argv(2, filename, sizeof(filename));
        }
        G_ReplaySave(filename);
    } else if (Q_stricmp(cmd, "load") == 0) {
        char filename[64] = "neonwave_replay.dat";
        if (trap_Argc() >= 3) {
            trap_Argv(2, filename, sizeof(filename));
        }
        G_ReplayLoad(filename);
    } else if (Q_stricmp(cmd, "play") == 0) {
        G_ReplayPlayStart();
    } else if (Q_stricmp(cmd, "status") == 0) {
        G_Printf("Replay: %s, %d events\n",
                 replayRecording ? "recording" : (replayPlaying ? "playing" : "idle"),
                 replayCount);
    } else {
        G_Printf("Unknown replay command: %s\n", cmd);
    }
}

#endif // NEONARENA_MOD
