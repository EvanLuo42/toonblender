/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_common_color_utils.bsl.hh"
#include "gpu_shader_material_interface.bsl.hh"
#include "gpu_shader_material_open_pbr_util.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

float3 toon_surface_sample_flattened_lut(sampler2D lut_tx, float3 color)
{
  const int2 extent = textureSize(lut_tx, 0).xy;
  const int lut_size = extent.y;
  if (lut_size <= 1 || extent.x != lut_size * lut_size) {
    return color;
  }

  /* Character LUTs are authored in sRGB. Scene-linear albedo would otherwise
   * sample the near-black corner of the volume and crush the result. */
  const float3 lut_coord = saturate(linear_rgb_to_srgb(max(color, float3(0.0f)))) *
                           float(lut_size - 1);
  const float blue_lower = floor(lut_coord.b);
  const float blue_upper = min(blue_lower + 1.0f, float(lut_size - 1));
  const float2 texel_center = float2(0.5f);

  const float2 uv_lower = (float2(lut_coord.r + blue_lower * float(lut_size), lut_coord.g) +
                           texel_center) /
                          float2(extent);
  const float2 uv_upper = (float2(lut_coord.r + blue_upper * float(lut_size), lut_coord.g) +
                           texel_center) /
                          float2(extent);
  const float3 lower = textureLod(lut_tx, uv_lower, 0.0f).rgb;
  const float3 upper = textureLod(lut_tx, uv_upper, 0.0f).rgb;
  return mix(lower, upper, fract(lut_coord.b));
}

[[node]]
void toon_surface_diffuse_color(float4 base_color, float4 diffuse_texture, float4 &result)
{
  result = max(base_color * diffuse_texture, float4(0.0f));
}

[[node]]
void toon_surface_diffuse_lut(float4 diffuse_color,
                              float influence,
                              sampler2D lut_tx,
                              float4 &result)
{
  const float3 lut_color = toon_surface_sample_flattened_lut(lut_tx, diffuse_color.rgb);
  result = float4(mix(diffuse_color.rgb, lut_color, saturate(influence)), diffuse_color.a);
}

[[node]]
void node_bsdf_toon_surface_direct(float light_index,
                                   float4 diffuse_color,
                                   float metallic,
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
                               weight * saturate(alpha) * (1.0f - saturate(metallic)),
                               result);
}

float3 toon_surface_eval_gloss(const float3 weight,
                               const Specular specular,
                               const float3 N,
                               const float NV,
                               const bool multiggx,
                               ClosureReflection &reflection_data)
{
  const float ior = openpbr_modulate_ior(specular.weight, specular.ior);
  const float3 F0 = saturate(float3(F0_from_ior(ior)) * specular.tint);
  const float3 F90 = float3(1.0f);

  float3 reflectance, unused;
  bsdf_lut(F0, F90, float3(0.0f), NV, specular.roughness, ior, multiggx, reflectance, unused);

  reflection_data.N = N;
  reflection_data.roughness = specular.roughness;
  reflection_data.color += weight * reflectance;
  closure_eval(reflection_data);

  return weight * max((1.0f - math_reduce_max(reflectance)), 0.0f);
}

[[node]]
void node_bsdf_toon_surface(float4 base_color,
                            float metallic,
                            const float roughness,
                            const float ior,
                            float alpha,
                            float3 N,
                            const float float_weight,
                            [[maybe_unused]] const float diffuse_warp,
                            [[maybe_unused]] const float4 diffuse_texture,
                            [[maybe_unused]] const float ao,
                            [[maybe_unused]] const float diffuse_lut_influence,
                            const float specular_ior_level,
                            const float4 specular_tint,
                            const float sheen_weight,
                            const float sheen_roughness,
                            const float4 sheen_tint,
                            const float coat_weight,
                            const float coat_roughness,
                            const float coat_ior,
                            const float4 coat_tint,
                            const float3 CN,
                            const float do_multiscatter,
                            [[maybe_unused]] const float direct_weight,
                            [[maybe_unused]] const float4 toon_diffuse_color,
                            Closure &result)
{
  metallic = saturate(metallic);
  alpha = saturate(alpha);
  base_color = max(base_color, float4(0.0f));
  const float3 clamped_base_color = min(base_color.rgb, float3(1.0f));

  Specular specular;
  specular.tint = max(specular_tint.rgb, float3(0.0f));
  specular.roughness = saturate(roughness);
  specular.ior = max(ior, 1e-5f);
  specular.weight = max(specular_ior_level * 2.0f, 0.0f);

  Coat coat;
  coat.tint = max(coat_tint.rgb, float3(0.0f));
  coat.roughness = saturate(coat_roughness);
  coat.N = normalize_fallback(CN, g_data.N);
  coat.ior = max(coat_ior, 1.0f);
  coat.weight = saturate(coat_weight);

  Fuzz fuzz;
  fuzz.tint = max(sheen_tint.rgb, float3(0.0f));
  fuzz.roughness = saturate(sheen_roughness);
  fuzz.weight = max(sheen_weight, 0.0f);

  N = normalize_fallback(N, g_data.N);
  const float3 V = coordinate_incoming(g_data.P);
  const float NV = dot(N, V);
  const bool multiggx = do_multiscatter != 0.0f;

  float3 weight = float3(float_weight);
  ClosureDiffuse fuzz_data;
  ClosureReflection reflection_data;

  weight = openpbr_eval_transparency(weight, alpha);
  weight = openpbr_eval_fuzz(weight, coat, fuzz, N, V, fuzz_data);
  weight = openpbr_eval_coat(weight, coat, V);
  weight = openpbr_eval_metal(
      weight, clamped_base_color, specular, metallic, NV, multiggx, reflection_data);
  weight = toon_surface_eval_gloss(weight, specular, N, NV, multiggx, reflection_data);

#ifdef MAT_DIFFUSE
  ClosureToonDiffuse diffuse_data;
  diffuse_data.N = N;
  diffuse_data.color = fuzz_data.color + weight * toon_diffuse_color.rgb * saturate(ao);
  diffuse_data.warp = saturate(diffuse_warp);
  diffuse_data.direct_weight = direct_weight;
  closure_eval(diffuse_data);
#endif

  /* Closure data is accumulated as a side effect by EEVEE. */
  result = CLOSURE_DEFAULT;
}
