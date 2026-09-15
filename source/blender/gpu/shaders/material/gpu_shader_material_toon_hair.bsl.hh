/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_material_toon_ramp_lib.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

float3 toon_hair_tangent(const float3 T, const float3 N)
{
  if (g_data.is_strand) {
    return normalize_fallback(g_data.curve_T, T);
  }
  const float3 world_up = float3(0.0f, 0.0f, 1.0f);
  const float3 generated = cross(
      N, normalize_fallback(cross(world_up, N), float3(1.0f, 0.0f, 0.0f)));
  /* HN run through a Normal Map node is almost parallel to N, which zeros the
   * gram-schmidt and kills Kajiya-Kay. Treat that as "no tangent". */
  const float3 candidate = normalize_fallback(T, generated);
  const float3 tangent = (abs(dot(candidate, N)) > 0.95f) ? generated : candidate;
  return normalize_fallback(tangent - N * dot(tangent, N), generated);
}

float3 toon_hair_shift_tangent(const float3 T, const float3 N, const float shift)
{
  return normalize_fallback(T + N * shift, T);
}

float toon_hair_exponent(const float roughness)
{
  return exp2(mix(10.0f, 2.0f, saturate(roughness)));
}

/* Kajiya-Kay strand specular: sin(T, H) with a directional fade so the even sin lobe
 * does not light the back of the strand. */
float toon_hair_strand_specular(const float3 T, const float3 H, const float exponent)
{
  const float TdotH = dot(T, H);
  const float sinTH = sqrt(max(1.0f - TdotH * TdotH, 0.0f));
  const float dir_atten = smoothstep(-1.0f, 0.0f, TdotH);
  return dir_atten * pow(sinTH, max(exponent, 1.0e-5f));
}

float toon_hair_sin_th(const float3 T, const float3 H, float &dir_atten)
{
  const float TdotH = dot(T, H);
  dir_atten = smoothstep(-1.0f, 0.0f, TdotH);
  return sqrt(max(1.0f - TdotH * TdotH, 0.0f));
}

float3 toon_hair_eval_lobe(const float3 T,
                           const float3 N,
                           const float3 H,
                           const float shift,
                           const float roughness,
                           const float weight,
                           const float3 tint,
                           const float use_spec_ramp,
                           sampler2D spec_ramp_tx)
{
  const float3 Ts = toon_hair_shift_tangent(T, N, shift);
  if (use_spec_ramp > 0.5f) {
    float dir_atten;
    const float sinTH = toon_hair_sin_th(Ts, H, dir_atten);
    const float4 spec_ramp = toon_sample_ramp(spec_ramp_tx, sinTH, saturate(roughness));
    return max(spec_ramp.rgb, float3(0.0f)) * saturate(spec_ramp.a) * dir_atten * weight *
           max(tint, float3(0.0f));
  }
  return float3(toon_hair_strand_specular(Ts, H, toon_hair_exponent(roughness)) * weight) *
         max(tint, float3(0.0f));
}

[[node]]
void node_bsdf_toon_hair_direct(float light_index,
                                float4 diffuse_color,
                                float4 shadow_color,
                                float roughness,
                                float alpha,
                                float ao,
                                float3 N,
                                float3 T,
                                float weight,
                                float diffuse_warp,
                                float spec_weight,
                                float4 spec_tint,
                                float spec_shift,
                                float secondary_weight,
                                float4 secondary_tint,
                                float secondary_shift,
                                float secondary_roughness,
                                float shift,
                                float use_diffuse_ramp,
                                float use_spec_ramp,
                                sampler2D ramp_tx,
                                sampler2D spec_ramp_tx,
                                Closure &result)
{
  const int index = int(light_index);
  N = normalize_fallback(N, g_data.N);
  T = toon_hair_tangent(T, N);

  float3 direction;
  float3 light_radiance;
  node_toon_light_evaluation_impl(index, g_data.P, direction, light_radiance);

  float4 shadow;
  node_shadow_raycast_impl(index, g_data.P, 1.0f, shadow);

  const float nl = saturate(mix(dot(N, direction), 1.0f, saturate(diffuse_warp)));
  float3 ramp_rgb = float3(nl);
  float ramp_alpha = 1.0f;
  if (use_diffuse_ramp > 0.5f) {
    const float4 ramp = toon_sample_ramp(ramp_tx, nl, 0.5f);
    ramp_rgb = ramp.rgb;
    ramp_alpha = ramp.a;
  }

  const float3 albedo = toon_ramp_mix_albedo(
      diffuse_color.rgb, shadow_color.rgb, ramp_alpha, ao);
  const float3 diffuse_light = max(light_radiance * shadow.rgb * ramp_rgb, float3(0.0f));

  const float3 V = coordinate_incoming(g_data.P);
  const float3 H = normalize_fallback(direction + V, N);
  const float tex_shift = shift - 0.5f;
  const float3 spec_contrib =
      toon_hair_eval_lobe(T,
                          N,
                          H,
                          spec_shift + tex_shift,
                          roughness,
                          saturate(spec_weight),
                          spec_tint.rgb,
                          use_spec_ramp,
                          spec_ramp_tx) +
      toon_hair_eval_lobe(T,
                          N,
                          H,
                          secondary_shift + tex_shift,
                          secondary_roughness,
                          saturate(secondary_weight),
                          secondary_tint.rgb,
                          use_spec_ramp,
                          spec_ramp_tx);
  const float3 glossy_light = max(light_radiance * shadow.rgb * spec_contrib, float3(0.0f));

  node_light_accumulation_impl(index,
                               diffuse_light,
                               albedo,
                               glossy_light,
                               float3(1.0f),
                               float3(0.0f),
                               float3(0.0f),
                               weight * saturate(alpha),
                               result);
}

[[node]]
void node_bsdf_toon_hair(const float4 base_color,
                         const float roughness,
                         float alpha,
                         float3 N,
                         float3 T,
                         const float float_weight,
                         const float diffuse_warp,
                         const float ao,
                         [[maybe_unused]] const float4 shadow_color,
                         const float spec_weight,
                         const float4 spec_tint,
                         const float spec_shift,
                         const float secondary_weight,
                         const float4 secondary_tint,
                         const float secondary_shift,
                         const float secondary_roughness,
                         const float shift,
                         const float rim_weight,
                         const float4 rim_tint,
                         const float rim_exponent,
                         const float direct_weight,
                         Closure &result)
{
  alpha = saturate(alpha);
  N = normalize_fallback(N, g_data.N);
  T = toon_hair_tangent(T, N);

  float3 weight = float3(float_weight);

  ClosureTransparency transparency_data;
  transparency_data.transmittance = (1.0f - alpha) * weight;
  transparency_data.holdout = 0.0f;
  closure_eval(transparency_data);
  weight *= alpha;

#ifdef MAT_DIFFUSE
  ClosureToonDiffuse diffuse_data;
  diffuse_data.N = N;
  diffuse_data.color = weight * saturate(base_color.rgb) * saturate(ao);
  diffuse_data.warp = saturate(diffuse_warp);
  diffuse_data.direct_weight = direct_weight;
  closure_eval(diffuse_data);
#endif

  /* View-locked anisotropic highlight (天使环). A fake key light from above the
   * camera keeps the band on top of the hair even when there is no sun. */
  const float3 V = coordinate_incoming(g_data.P);
  const float3 L = normalize_fallback(V + float3(0.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 0.0f));
  const float3 H = normalize_fallback(L + V, N);
  const float tex_shift = shift - 0.5f;
  const float3 spec =
      float3(toon_hair_strand_specular(toon_hair_shift_tangent(T, N, spec_shift + tex_shift),
                                       H,
                                       toon_hair_exponent(roughness))) *
          saturate(spec_weight) * max(spec_tint.rgb, float3(0.0f)) +
      float3(toon_hair_strand_specular(
                 toon_hair_shift_tangent(T, N, secondary_shift + tex_shift),
                 H,
                 toon_hair_exponent(secondary_roughness))) *
          saturate(secondary_weight) * max(secondary_tint.rgb, float3(0.0f));
  if (math_reduce_max(spec) > 1.0e-5f) {
    ClosureEmission spec_data;
    spec_data.emission = weight * spec;
    closure_eval(spec_data);
  }

  const float rim = saturate(rim_weight) *
                    pow(saturate(1.0f - saturate(dot(N, V))), max(rim_exponent, 1.0e-5f));
  if (rim > 1.0e-5f) {
    ClosureEmission emission_data;
    emission_data.emission = weight * rim * max(rim_tint.rgb, float3(0.0f));
    closure_eval(emission_data);
  }

  /* Closure data is accumulated as a side effect by EEVEE. */
  result = CLOSURE_DEFAULT;
}
