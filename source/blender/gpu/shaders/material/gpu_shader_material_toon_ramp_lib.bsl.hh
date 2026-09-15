/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

float4 toon_sample_ramp(sampler2D ramp_tx, const float u, const float v)
{
  const int2 extent = max(textureSize(ramp_tx, 0).xy, int2(1));
  const float2 uv = float2((saturate(u) * float(extent.x - 1) + 0.5f) / float(extent.x),
                           (saturate(v) * float(extent.y - 1) + 0.5f) / float(extent.y));
  return textureLod(ramp_tx, uv, 0.0f);
}

float3 toon_ramp_mix_albedo(const float3 base_color,
                            const float3 shadow_color,
                            const float ramp_alpha,
                            const float ao)
{
  return mix(max(shadow_color, float3(0.0f)), saturate(base_color), saturate(ramp_alpha)) *
         saturate(ao);
}

float toon_ramp_shade(const float4 ramp)
{
  return saturate(dot(max(ramp.rgb, float3(0.0f)), float3(1.0f / 3.0f)) * saturate(ramp.a));
}

float3 toon_ramp_tint(const float4 ramp)
{
  const float lum = dot(max(ramp.rgb, float3(0.0f)), float3(1.0f / 3.0f));
  return (lum > 1.0e-5f) ? (max(ramp.rgb, float3(0.0f)) / lum) : float3(1.0f);
}

float3 toon_skin_mix_albedo(const float3 lit_color,
                            const float3 dark_color,
                            const float inner_dark,
                            const float shade,
                            const float nl,
                            const float inner_threshold,
                            const float ao)
{
  const float3 lit = saturate(lit_color);
  const float3 dark = max(dark_color, float3(0.0f));
  const float3 inner = dark * saturate(inner_dark);
  const float3 first_shadow = mix(dark, lit, saturate(shade));
  /* A zero threshold disables the inner-dark mix. */
  const float t = saturate(inner_threshold);
  const float inner_mask = (t <= 1.0e-5f) ? 1.0f : saturate(nl / t);
  return mix(inner, first_shadow, inner_mask) * saturate(ao);
}

float3 toon_skin_sss_radius(const float3 radius, const float scale)
{
  return max(radius * max(scale, 0.0f), float3(0.0f));
}

float3 toon_skin_sss_tint(const float3 radius)
{
  const float3 positive = max(radius, float3(0.0f));
  const float peak = max(max(positive.r, positive.g), positive.b);
  return (peak > 1.0e-5f) ? (positive / peak) : float3(1.0f);
}

float toon_skin_sss_wrap(const float sss_weight, const float sss_scale)
{
  /* Scale is a world-space radius; a few centimeters add a small extra wrap. */
  return saturate(sss_weight) * saturate(sss_scale * 2.0f);
}

float3 toon_skin_sss_albedo(const float3 albedo,
                            const float3 sss_tint,
                            const float sss_weight,
                            const float shade)
{
  /* Chromatic scatter in the unlit region, matching the red terminator of skin. */
  return mix(albedo, albedo * sss_tint, saturate(sss_weight) * (1.0f - saturate(shade)));
}
