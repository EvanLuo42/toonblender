/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_material_toon_ramp_lib.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

[[node]]
void toon_skin_scale_color(float4 color, float scale, float4 &result)
{
  result = float4(max(color.rgb, float3(0.0f)) * max(scale, 0.0f), color.a);
}

[[node]]
void node_bsdf_toon_skin_direct(float light_index,
                                float4 diffuse_color,
                                float4 dark_color,
                                float inner_dark,
                                float inner_threshold,
                                float alpha,
                                float ao,
                                float3 N,
                                float weight,
                                float diffuse_warp,
                                float sss_weight,
                                float3 sss_radius,
                                float sss_scale,
                                sampler2D ramp_tx,
                                Closure &result)
{
  const int index = int(light_index);
  N = normalize_fallback(N, g_data.N);
  sss_weight = saturate(sss_weight);

  float3 direction;
  float3 light_radiance;
  node_toon_light_evaluation_impl(index, g_data.P, direction, light_radiance);

  float4 shadow;
  node_shadow_raycast_impl(index, g_data.P, 1.0f, shadow);

  const float wrap = saturate(saturate(diffuse_warp) +
                              toon_skin_sss_wrap(sss_weight, sss_scale));
  const float nl = saturate(mix(dot(N, direction), 1.0f, wrap));
  const float4 ramp = toon_sample_ramp(ramp_tx, nl, 0.5f);
  const float shade = toon_ramp_shade(ramp);
  const float3 sss_tint = toon_skin_sss_tint(sss_radius);
  const float3 albedo = toon_skin_sss_albedo(toon_skin_mix_albedo(diffuse_color.rgb,
                                                                 dark_color.rgb,
                                                                 inner_dark,
                                                                 shade,
                                                                 nl,
                                                                 inner_threshold,
                                                                 ao),
                                             sss_tint,
                                             sss_weight,
                                             shade);
  const float3 diffuse_light = max(
      light_radiance * shadow.rgb * toon_ramp_tint(ramp), float3(0.0f));
  /* Back-facing transmission through thin features such as ears. */
  const float3 transmission_light = max(
      light_radiance * shadow.rgb * saturate(-dot(N, direction)), float3(0.0f));
  const float3 transmission_color = sss_tint * sss_weight;
  node_light_accumulation_impl(index,
                               diffuse_light,
                               albedo,
                               float3(0.0f),
                               float3(0.0f),
                               transmission_light,
                               transmission_color,
                               weight * saturate(alpha),
                               result);
}

[[node]]
void node_bsdf_toon_skin(const float4 base_color,
                         float alpha,
                         float3 N,
                         const float float_weight,
                         const float diffuse_warp,
                         const float ao,
                         [[maybe_unused]] const float lut_influence,
                         [[maybe_unused]] const float dark_strength,
                         [[maybe_unused]] const float inner_dark,
                         [[maybe_unused]] const float inner_threshold,
                         [[maybe_unused]] float sss_weight,
                         [[maybe_unused]] const float3 sss_radius,
                         [[maybe_unused]] const float sss_scale,
                         const float rim_weight,
                         const float4 rim_tint,
                         const float rim_exponent,
                         const float direct_weight,
                         Closure &result)
{
  alpha = saturate(alpha);
  N = normalize_fallback(N, g_data.N);
  sss_weight = saturate(sss_weight);

  float3 weight = float3(float_weight);

  ClosureTransparency transparency_data;
  transparency_data.transmittance = (1.0f - alpha) * weight;
  transparency_data.holdout = 0.0f;
  closure_eval(transparency_data);
  weight *= alpha;

  const float3 albedo = saturate(base_color.rgb) * saturate(ao);
  float3 diffuse_weight = weight;

#ifdef MAT_SUBSURFACE
  /* Ramp lighting already evaluates local lights. Emitting a BSSRDF there would add Lambert
   * lighting on top. Screen-space SSS is used when the closure path owns local lights. */
  if (sss_weight > 0.0f && direct_weight > 0.5f) {
    ClosureSubsurface sss_data;
    sss_data.N = N;
    sss_data.sss_radius = toon_skin_sss_radius(sss_radius, sss_scale);
    sss_data.color = diffuse_weight * sss_weight * albedo;
    closure_eval(sss_data);
    diffuse_weight *= (1.0f - sss_weight);
  }
#endif

#ifdef MAT_DIFFUSE
  ClosureToonDiffuse diffuse_data;
  diffuse_data.N = N;
  diffuse_data.color = diffuse_weight * albedo;
  /* Extra wrap approximates scatter when screen-space SSS is not used (ramp lighting). */
  const float wrap_extra = (direct_weight > 0.5f) ?
                               0.0f :
                               toon_skin_sss_wrap(sss_weight, sss_scale);
  diffuse_data.warp = saturate(diffuse_warp + wrap_extra);
  diffuse_data.direct_weight = direct_weight;
  closure_eval(diffuse_data);
#endif

  const float rim = saturate(rim_weight) *
                    pow(saturate(1.0f - saturate(dot(N, coordinate_incoming(g_data.P)))),
                        max(rim_exponent, 1.0e-5f));
  if (rim > 1.0e-5f) {
    ClosureEmission emission_data;
    emission_data.emission = weight * rim * max(rim_tint.rgb, float3(0.0f));
    closure_eval(emission_data);
  }

  /* Closure data is accumulated as a side effect by EEVEE. */
  result = CLOSURE_DEFAULT;
}
