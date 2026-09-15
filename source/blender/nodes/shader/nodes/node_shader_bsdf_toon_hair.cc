/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

namespace blender {

namespace nodes::node_shader_bsdf_toon_hair_cc {

enum ToonHairMode {
  TOON_HAIR_MODE_CLOSURE = 0,
  TOON_HAIR_MODE_DIRECT_LIGHT = 1,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.use_custom_socket_order();

  b.add_output<decl::Shader>("BSDF"_ustr);

  b.add_input<decl::Color>("Base Color"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f})
      .description("Hair albedo; connect the diffuse texture here");
#define TOON_HAIR_SOCK_BASE_COLOR_ID 0
  b.add_input<decl::Float>("Roughness"_ustr)
      .default_value(0.3f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Primary highlight roughness. Selects V on Ramp Specular, or the Kajiya-Kay "
          "exponent when the ramp is empty");
#define TOON_HAIR_SOCK_ROUGHNESS_ID 1
  b.add_input<decl::Float>("Alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Hair opacity; use the alpha of hair cards here");
#define TOON_HAIR_SOCK_ALPHA_ID 2
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
#define TOON_HAIR_SOCK_NORMAL_ID 3
  b.add_input<decl::Vector>("Tangent"_ustr)
      .hide_value()
      .description(
          "Hair strand tangent from root to tip. Defaults to the UV tangent, or the curve "
          "tangent on hair curves");
#define TOON_HAIR_SOCK_TANGENT_ID 4
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
#define TOON_HAIR_SOCK_WEIGHT_ID 5

  PanelDeclarationBuilder &diffuse = b.add_panel("Diffuse"_ustr).default_closed(false);
  diffuse.add_input<decl::Float>("Diffuse Warp"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Wrap diffuse illumination around the surface; 0 is Lambert and 0.5 is Half-Lambert");
#define TOON_HAIR_SOCK_DIFFUSE_WARP_ID 6
  diffuse.add_input<decl::Float>("AO"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Ambient occlusion applied to the diffuse component");
#define TOON_HAIR_SOCK_AO_ID 7
  diffuse.add_input<decl::Color>("Shadow Color"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .description("Albedo used where the diffuse ramp alpha is 0");
#define TOON_HAIR_SOCK_SHADOW_COLOR_ID 8
  diffuse.add_input<decl::Image>("Ramp Texture"_ustr)
      .description(
          "Hair lighting ramp. RGB tints the light; alpha blends Shadow Color (0) with Base "
          "Color (1)");
#define TOON_HAIR_SOCK_RAMP_TEXTURE_ID 9

  PanelDeclarationBuilder &specular = b.add_panel("Specular"_ustr).default_closed(false);
  specular.add_input<decl::Float>("Specular Weight"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the primary Kajiya-Kay highlight");
#define TOON_HAIR_SOCK_SPEC_WEIGHT_ID 10
  specular.add_input<decl::Color>("Specular Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Color of the primary highlight");
#define TOON_HAIR_SOCK_SPEC_TINT_ID 11
  specular.add_input<decl::Float>("Specular Shift"_ustr)
      .default_value(-0.1f)
      .min(-1.0f)
      .max(1.0f)
      .short_label("Shift"_ustr)
      .description(
          "Shifts the primary highlight along the strand; negative moves it toward the root");
#define TOON_HAIR_SOCK_SPEC_SHIFT_ID 12

  PanelDeclarationBuilder &secondary = b.add_panel("Secondary"_ustr).default_closed(false);
  secondary.add_input<decl::Float>("Secondary Weight"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the secondary Kajiya-Kay highlight");
#define TOON_HAIR_SOCK_SECONDARY_WEIGHT_ID 13
  secondary.add_input<decl::Color>("Secondary Tint"_ustr)
      .default_value({1.0f, 0.8f, 0.6f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Color of the secondary highlight");
#define TOON_HAIR_SOCK_SECONDARY_TINT_ID 14
  secondary.add_input<decl::Float>("Secondary Shift"_ustr)
      .default_value(0.2f)
      .min(-1.0f)
      .max(1.0f)
      .short_label("Shift"_ustr)
      .description(
          "Shifts the secondary highlight along the strand; positive moves it toward the tip");
#define TOON_HAIR_SOCK_SECONDARY_SHIFT_ID 15
  secondary.add_input<decl::Float>("Secondary Roughness"_ustr)
      .default_value(0.6f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Roughness"_ustr)
      .description(
          "Width of the secondary highlight. Independent of the primary Roughness used for "
          "Ramp Specular V");
#define TOON_HAIR_SOCK_SECONDARY_ROUGHNESS_ID 16

  specular.add_input<decl::Float>("Shift Map"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("ST"_ustr)
      .description(
          "Per-pixel tangent shift from the ST texture. 0.5 adds no extra shift. Added to both "
          "specular lobes");
#define TOON_HAIR_SOCK_SHIFT_ID 17
  specular.add_input<decl::Image>("Ramp Specular"_ustr)
      .description(
          "Optional Kajiya-Kay specular ramp sampled by sin(T, H) on U and primary Roughness "
          "on V. Leave empty to use an analytical highlight");
#define TOON_HAIR_SOCK_RAMP_SPECULAR_ID 18

  PanelDeclarationBuilder &rim = b.add_panel("Rim"_ustr).default_closed(true);
  rim.add_input<decl::Float>("Rim Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the view-facing rim light");
#define TOON_HAIR_SOCK_RIM_WEIGHT_ID 19
  rim.add_input<decl::Color>("Rim Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Color of the rim light");
#define TOON_HAIR_SOCK_RIM_TINT_ID 20
  rim.add_input<decl::Float>("Rim Exponent"_ustr)
      .default_value(5.0f)
      .min(0.0f)
      .max(20.0f)
      .short_label("Exponent"_ustr)
      .description("Tightness of the rim; higher values confine the rim to glancing angles");
#define TOON_HAIR_SOCK_RIM_EXPONENT_ID 21

  b.add_input<decl::Int>("LightIndex"_ustr).available(is_gpu_internal);
#define TOON_HAIR_SOCK_LIGHT_INDEX_ID 22
}

static void node_shader_init_toon_hair(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderToonHair *storage = MEM_new<NodeShaderToonHair>(__func__);
  BKE_imageuser_default(&storage->ramp_iuser);
  BKE_imageuser_default(&storage->spec_ramp_iuser);
  node->storage = storage;
  node->custom1 = TOON_HAIR_MODE_CLOSURE;
}

static Image *image_from_socket(bNode &node, const UString identifier)
{
  bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueImage>()->value : nullptr;
}

static GPUNodeLink *ensure_link(const GPUNodeStack &socket)
{
  /* GPU_link() frees non-output links. Do not store temporaries back on the socket stack,
   * otherwise later GPU_stack_link() walks a dangling pointer. */
  return socket.link ? socket.link : GPU_uniform(socket);
}

static int node_shader_gpu_bsdf_toon_hair(GPUMaterial *mat,
                                          bNode *node,
                                          bNodeExecData * /*execdata*/,
                                          GPUNodeStack *in,
                                          GPUNodeStack *out)
{
  bNode *node_original = node->runtime->original ? node->runtime->original : node;
  NodeShaderToonHair *storage_original = static_cast<NodeShaderToonHair *>(
      node_original->storage);

  Image *ramp_image = image_from_socket(*node, "Ramp Texture"_ustr);
  Image *spec_ramp_image = image_from_socket(*node, "Ramp Specular"_ustr);

  if (!in[TOON_HAIR_SOCK_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[TOON_HAIR_SOCK_NORMAL_ID].link);
  }
  if (!in[TOON_HAIR_SOCK_TANGENT_ID].link) {
    GPU_link(mat,
             "node_tangentmap",
             GPU_attribute(mat, CD_TANGENT, ""),
             &in[TOON_HAIR_SOCK_TANGENT_ID].link);
  }

  GPUSamplerState lookup_sampler = GPUSamplerState::default_sampler();
  lookup_sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR;
  lookup_sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
  lookup_sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;

  if (node->custom1 == TOON_HAIR_MODE_DIRECT_LIGHT) {
    if (!ramp_image && !spec_ramp_image) {
      return false;
    }

    eGPUMaterialFlag lighting_flag = GPU_MATFLAG_LIGHTING | GPU_MATFLAG_DIFFUSE;
    if (in[TOON_HAIR_SOCK_SPEC_WEIGHT_ID].socket_not_zero() ||
        in[TOON_HAIR_SOCK_SECONDARY_WEIGHT_ID].socket_not_zero() || spec_ramp_image)
    {
      lighting_flag |= GPU_MATFLAG_GLOSSY;
    }
    GPU_material_flag_set(mat, lighting_flag);

    Image *diffuse_bind = ramp_image ? ramp_image : spec_ramp_image;
    Image *spec_bind = spec_ramp_image ? spec_ramp_image : ramp_image;
    ImageUser *diffuse_iuser = ramp_image ? &storage_original->ramp_iuser :
                                            &storage_original->spec_ramp_iuser;
    ImageUser *spec_iuser = spec_ramp_image ? &storage_original->spec_ramp_iuser :
                                              &storage_original->ramp_iuser;
    const float use_diffuse_ramp = ramp_image ? 1.0f : 0.0f;
    const float use_spec_ramp = spec_ramp_image ? 1.0f : 0.0f;

    return GPU_link(mat,
                    "node_bsdf_toon_hair_direct",
                    ensure_link(in[TOON_HAIR_SOCK_LIGHT_INDEX_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_BASE_COLOR_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SHADOW_COLOR_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_ROUGHNESS_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_ALPHA_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_AO_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_NORMAL_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_TANGENT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_WEIGHT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_DIFFUSE_WARP_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SPEC_WEIGHT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SPEC_TINT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SPEC_SHIFT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SECONDARY_WEIGHT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SECONDARY_TINT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SECONDARY_SHIFT_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SECONDARY_ROUGHNESS_ID]),
                    ensure_link(in[TOON_HAIR_SOCK_SHIFT_ID]),
                    GPU_constant(&use_diffuse_ramp),
                    GPU_constant(&use_spec_ramp),
                    GPU_image(mat, diffuse_bind, diffuse_iuser, lookup_sampler),
                    GPU_image(mat, spec_bind, spec_iuser, lookup_sampler),
                    &out[0].link);
  }

  const bool use_transparency = in[TOON_HAIR_SOCK_ALPHA_ID].socket_not_one();
  const bool use_rim = in[TOON_HAIR_SOCK_RIM_WEIGHT_ID].socket_not_zero();
  const bool use_spec = in[TOON_HAIR_SOCK_SPEC_WEIGHT_ID].socket_not_zero() ||
                        in[TOON_HAIR_SOCK_SECONDARY_WEIGHT_ID].socket_not_zero();

  eGPUMaterialFlag flag = GPU_MATFLAG_DIFFUSE;
  if (use_transparency) {
    flag |= GPU_MATFLAG_TRANSPARENT;
  }
  if (use_rim || use_spec) {
    flag |= GPU_MATFLAG_EMISSION;
  }

  /* Toon diffuse stores its warp in an additional G-buffer layer. Marking reflection as maybe
   * colored prevents the two-closure simple layout from dropping that layer. */
  flag |= GPU_MATFLAG_REFLECTION_MAYBE_COLORED;

  GPU_material_flag_set(mat, flag);

  /* LightIndex is only used by the direct-light GPU function. Leaving it on the stack would
   * make GPU_stack_link() pass it as a node_bsdf_toon_hair argument and shift the extra
   * parameter (direct weight). */
  for (int i = 0; !in[i].end; i++) {
    if (i == TOON_HAIR_SOCK_LIGHT_INDEX_ID) {
      in[i].type = GPU_NONE;
      break;
    }
  }

  const float direct_weight = (ramp_image || spec_ramp_image) ? 0.0f : 1.0f;
  return GPU_stack_link(mat, node, "node_bsdf_toon_hair", in, out, GPU_constant(&direct_weight));
}

}  // namespace nodes::node_shader_bsdf_toon_hair_cc

void register_node_type_sh_bsdf_toon_hair()
{
  namespace file_ns = nodes::node_shader_bsdf_toon_hair_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfToonHair"_ustr, SH_NODE_BSDF_TOON_HAIR);
  ntype.ui_name = "Toon Hair BSDF";
  ntype.ui_description =
      "Eevee toon hair shader using ramp lighting and dual-lobe Kajiya-Kay specular, with an "
      "optional specular ramp";
  ntype.enum_name_legacy = "BSDF_TOON_HAIR";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_shader_init_toon_hair;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_toon_hair;
  bke::node_type_storage(
      ntype, "NodeShaderToonHair", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
