// SPDX-License-Identifier: MIT
// Thin adapter over the Animation Compression Library (Nicholas Frechette, MIT).
#include <acl/core/ansi_allocator.h>
#include <acl/decompression/decompress.h>
#include <acl/decompression/decompression_settings.h>
#include <rtm/quatf.h>
#include <rtm/vector4f.h>
#include <cmath>
#include <cstdint>

namespace frontiers {

namespace {
const acl::compressed_tracks *open_clip(const uint8_t *blob, uint32_t size)
{
    if (!blob || size < 32 || (reinterpret_cast<uintptr_t>(blob) & 15)) return nullptr;
    acl::error_result error;
    const acl::compressed_tracks *clip = acl::make_compressed_tracks(blob, &error);
    if (!clip || error.any() || clip->is_valid(true).any()) return nullptr;
    if (clip->get_size() > size || clip->get_num_tracks() > 1024) return nullptr;
    return clip;
}

struct PoseWriter final : acl::track_writer {
    float *out;
    explicit PoseWriter(float *values) : out(values) {}
    void RTM_SIMD_CALL write_rotation(uint32_t track, rtm::quatf_arg0 q) { rtm::quat_store(q, out + track * 10 + 3); }
    void RTM_SIMD_CALL write_translation(uint32_t track, rtm::vector4f_arg0 v) { rtm::vector_store3(v, out + track * 10); }
    void RTM_SIMD_CALL write_scale(uint32_t track, rtm::vector4f_arg0 v) { rtm::vector_store3(v, out + track * 10 + 7); }
};
} // namespace

bool acl_clip_info(const uint8_t *blob, uint32_t size, uint32_t &tracks, uint32_t &samples, float &rate, float &duration)
{
    const acl::compressed_tracks *clip = open_clip(blob, size);
    if (!clip) return false;
    tracks = clip->get_num_tracks();
    samples = clip->get_num_samples_per_track();
    rate = clip->get_sample_rate();
    duration = clip->get_duration();
    return rate > 0 && std::isfinite(duration);
}

bool acl_sample(const uint8_t *blob, uint32_t size, float seconds, float *out, uint32_t capacity)
{
    const acl::compressed_tracks *clip = open_clip(blob, size);
    if (!clip || !out || !std::isfinite(seconds)) return false;
    const uint32_t count = clip->get_num_tracks();
    if (capacity < count * 10) return false;
    for (uint32_t i = 0; i < count; ++i) {
        float *p = out + i * 10;
        for (int j = 0; j < 10; ++j) p[j] = 0;
        p[6] = p[7] = p[8] = p[9] = 1;
    }
    acl::decompression_context<acl::default_transform_decompression_settings> context;
    if (!context.initialize(*clip)) return false;
    context.seek(seconds, acl::sample_rounding_policy::none);
    PoseWriter writer(out);
    context.decompress_tracks(writer);
    for (uint32_t i = 0; i < count * 10; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

} // namespace frontiers
