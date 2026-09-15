/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

[[node]]
void node_bsdf_toon_skin_direct(float light_index,
                                float4 diffuse_color,
                                float alpha,
                                float ao,
                                float3 N,
                                float weight,
                                float diffuse_warp,
                                sampler2D ramp_tx,
                                Closure &result)
{
  const int index = int(light_index);
  N = normalize_fallback(N, g_data.N);

  float3 direction;
  float3 light_radiance;
  node_toon_light_evaluation_impl(index, g_data.P, direction, light_radiance);

  float4 shadow;
  node_shadow_raycast_impl(index, g_data.P, 1.0f, shadow);

  const float ramp_coordinate = saturate(mix(dot(N, direction), 1.0f, saturate(diffuse_warp)));
  const int2 ramp_extent = max(textureSize(ramp_tx, 0).xy, int2(1));
  const float2 ramp_uv = float2(
      (ramp_coordinate * float(ramp_extent.x - 1) + 0.5f) / float(ramp_extent.x), 0.5f);
  const float3 ramp = textureLod(ramp_tx, ramp_uv, 0.0f).rgb;
  const float3 diffuse_light = max(light_radiance * shadow.rgb * ramp, float3(0.0f));
  const float3 albedo = saturate(diffuse_color.rgb) * saturate(ao);
  node_light_accumulation_impl(index,
                               diffuse_light,
                               albedo,
                               float3(0.0f),
                               float3(0.0f),
                               float3(0.0f),
                               float3(0.0f),
                               weight * saturate(alpha),
                               result);
}

[[node]]
void node_bsdf_toon_skin([[maybe_unused]] const float4 base_color,
                         float alpha,
                         float3 N,
                         const float float_weight,
                         const float diffuse_warp,
                         const float ao,
                         [[maybe_unused]] const float lut_influence,
                         const float rim_weight,
                         const float4 rim_tint,
                         const float rim_exponent,
                         [[maybe_unused]] const float direct_weight,
                         const float4 toon_diffuse_color,
                         Closure &result)
{
  alpha = saturate(alpha);
  N = normalize_fallback(N, g_data.N);

  float3 weight = float3(float_weight);

  ClosureTransparency transparency_data;
  transparency_data.transmittance = (1.0f - alpha) * weight;
  transparency_data.holdout = 0.0f;
  closure_eval(transparency_data);
  weight *= alpha;

#ifdef MAT_DIFFUSE
  ClosureToonDiffuse diffuse_data;
  diffuse_data.N = N;
  diffuse_data.color = weight * toon_diffuse_color.rgb * saturate(ao);
  diffuse_data.warp = saturate(diffuse_warp);
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
