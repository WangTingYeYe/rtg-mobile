#ifndef RTG_AUDIO_RENDERER_H
#define RTG_AUDIO_RENDERER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct RtgAudioRenderer RtgAudioRenderer;

enum { RTG_AUDIO_VOICE_SLOTS = 256 };
typedef struct {
    uint64_t played;
    uint32_t consumed;
    uint32_t dropped;
    uint32_t node;
    uint32_t used;
    uint64_t generation;
} RtgAudioVoiceCheckpoint;

typedef struct {
    uint64_t rendered_frames;
    uint64_t nonzero_frames;
    uint64_t source_frames;
    uint64_t updates;
    uint64_t invalid_parameters;
    uint64_t unsupported_parameters;
    uint32_t active_voices;
    uint32_t queued_wave_buffers;
    uint32_t active_effects;
    float peak;
} RtgAudioRendererMetrics;

enum {
    RTG_AUDIO_OK = 0,
    RTG_AUDIO_INVALID = -1,
    RTG_AUDIO_UNSUPPORTED = -2
};

/* SDK protocol revision used by this game's SDK. All calls must be serialized
 * by the caller. render() allocates no memory and calls no guest functions.
 * Source PCM storage must remain valid until its completion is published. */
RtgAudioRenderer *rtg_audio_renderer_create(void);
void rtg_audio_renderer_destroy(RtgAudioRenderer *renderer);
void rtg_audio_renderer_reset(RtgAudioRenderer *renderer);
int rtg_audio_renderer_update(RtgAudioRenderer *renderer,
    const uint8_t *channels, size_t channel_bytes,
    const uint8_t *voices, size_t voice_bytes,
    const uint8_t *mixes, size_t mix_bytes,
    const uint8_t *sinks, size_t sink_bytes);
/* Full SDK EffectInfo array (0xc0-byte records), applied before update().
 * I3dl2 uses a host Schroeder reverb approximation, not bit-exact DSP output. */
int rtg_audio_renderer_effects(RtgAudioRenderer *renderer,
    const uint8_t *effects, size_t effect_bytes);
/* Pending/paused wave storage must not be detached until this returns false.
 * Overflowing ranges conservatively count as overlapping. */
bool rtg_audio_renderer_uses_memory(const RtgAudioRenderer *renderer,
    uint64_t address, uint64_t size);
/* Interleaved stereo float PCM at 48000 Hz. */
void rtg_audio_renderer_render(RtgAudioRenderer *renderer, float *stereo,
    size_t frames);
/* 16-byte records: played source frames (u64), consumed buffers (u32),
 * dropped flag (u32), indexed by voice id. */
void rtg_audio_renderer_voice_status(const RtgAudioRenderer *renderer,
    uint8_t *out, size_t bytes);
/* Snapshot after rendering a queued PCM block. Publish these counters only
 * after the output callback consumes that entire block. Before writing SDK
 * status, compare used/generation against a fresh snapshot of the current
 * configuration; an older generation must never advance a reused voice id.
 * reset() preserves the monotonic generation sequence. Destroy/create starts
 * a new renderer lifetime, whose pending output blocks the caller must clear.
 * count is the number of elements; the complete snapshot has256 elements. */
void rtg_audio_renderer_voice_checkpoint(const RtgAudioRenderer *renderer,
    RtgAudioVoiceCheckpoint *out, size_t count);
RtgAudioRendererMetrics rtg_audio_renderer_metrics(const RtgAudioRenderer *renderer);

#endif
