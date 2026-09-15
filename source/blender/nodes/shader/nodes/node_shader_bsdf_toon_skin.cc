/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

namespace blender {

namespace nodes::node_shader_bsdf_toon_skin_cc {

enum ToonSkinMode {
  TOON_SKIN_MODE_CLOSURE = 0,
  TOON_SKIN_MODE_DIRECT_LIGHT = 1,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.use_custom_socket_order();

  b.add_output<decl::Shader>("BSDF"_ustr);

  b.add_input<decl::Color>("Base Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .description("Skin albedo; connect the diffuse texture here");
#define TOON_SKIN_SOCK_BASE_COLOR_ID 0
  b.add_input<decl::Float>("Alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Surface opacity");
#define TOON_SKIN_SOCK_ALPHA_ID 1
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
#define TOON_SKIN_SOCK_NORMAL_ID 2
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
#define TOON_SKIN_SOCK_WEIGHT_ID 3

  PanelDeclarationBuilder &diffuse = b.add_panel("Diffuse"_ustr).default_closed(false);
  diffuse.add_input<decl::Float>("Diffuse Warp"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Wrap diffuse illumination around the surface; 0 is Lambert and 0.5 is Half-Lambert");
#define TOON_SKIN_SOCK_DIFFUSE_WARP_ID 4
  diffuse.add_input<decl::Float>("AO"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Ambient occlusion applied to the diffuse component");
#define TOON_SKIN_SOCK_AO_ID 5
  diffuse.add_input<decl::Float>("LUT Influence"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Blend between the diffuse color and the color transformed by Skin LUT");
#define TOON_SKIN_SOCK_LUT_INFLUENCE_ID 6
  diffuse.add_input<decl::Image>("Ramp Texture"_ustr)
      .description("Horizontal lighting ramp sampled using the warped normal-light angle");
#define TOON_SKIN_SOCK_RAMP_TEXTURE_ID 7
  diffuse.add_input<decl::Image>("Skin LUT"_ustr)
      .description("Flattened 3D skin color LUT with dimensions N squared by N");
#define TOON_SKIN_SOCK_SKIN_LUT_ID 8

  PanelDeclarationBuilder &rim = b.add_panel("Rim"_ustr).default_closed(true);
  rim.add_input<decl::Float>("Rim Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the view-facing rim light");
#define TOON_SKIN_SOCK_RIM_WEIGHT_ID 9
  rim.add_input<decl::Color>("Rim Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Color of the rim light");
#define TOON_SKIN_SOCK_RIM_TINT_ID 10
  rim.add_input<decl::Float>("Rim Exponent"_ustr)
      .default_value(5.0f)
      .min(0.0f)
      .max(20.0f)
      .short_label("Exponent"_ustr)
      .description("Tightness of the rim; higher values confine the rim to glancing angles");
#define TOON_SKIN_SOCK_RIM_EXPONENT_ID 11

  b.add_input<decl::Int>("LightIndex"_ustr).available(is_gpu_internal);
#define TOON_SKIN_SOCK_LIGHT_INDEX_ID 12
}

static void node_shader_init_toon_skin(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderToonSkin *storage = MEM_new<NodeShaderToonSkin>(__func__);
  BKE_imageuser_default(&storage->ramp_iuser);
  BKE_imageuser_default(&storage->skin_lut_iuser);
  node->storage = storage;
  node->custom1 = TOON_SKIN_MODE_CLOSURE;
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

static int node_shader_gpu_bsdf_toon_skin(GPUMaterial *mat,
                                          bNode *node,
                                          bNodeExecData * /*execdata*/,
                                          GPUNodeStack *in,
                                          GPUNodeStack *out)
{
  bNode *node_original = node->runtime->original ? node->runtime->original : node;
  NodeShaderToonSkin *storage_original = static_cast<NodeShaderToonSkin *>(
      node_original->storage);

  Image *ramp_image = image_from_socket(*node, "Ramp Texture"_ustr);
  Image *skin_lut_image = image_from_socket(*node, "Skin LUT"_ustr);

  if (!in[TOON_SKIN_SOCK_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[TOON_SKIN_SOCK_NORMAL_ID].link);
  }

  GPUSamplerState lookup_sampler = GPUSamplerState::default_sampler();
  lookup_sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR;
  lookup_sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
  lookup_sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;

  GPUNodeLink *diffuse_color = ensure_link(in[TOON_SKIN_SOCK_BASE_COLOR_ID]);

  if (skin_lut_image) {
    GPUNodeLink *lut_color = nullptr;
    if (!GPU_link(mat,
                  "toon_surface_diffuse_lut",
                  diffuse_color,
                  ensure_link(in[TOON_SKIN_SOCK_LUT_INFLUENCE_ID]),
                  GPU_image(mat, skin_lut_image, &storage_original->skin_lut_iuser, lookup_sampler),
                  &lut_color))
    {
      return false;
    }
    diffuse_color = lut_color;
  }

  if (node->custom1 == TOON_SKIN_MODE_DIRECT_LIGHT) {
    if (!ramp_image) {
      return false;
    }

    GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTING | GPU_MATFLAG_DIFFUSE);
    GPUNodeLink *ramp_texture = GPU_image(
        mat, ramp_image, &storage_original->ramp_iuser, lookup_sampler);
    return GPU_link(mat,
                    "node_bsdf_toon_skin_direct",
                    ensure_link(in[TOON_SKIN_SOCK_LIGHT_INDEX_ID]),
                    diffuse_color,
                    ensure_link(in[TOON_SKIN_SOCK_ALPHA_ID]),
                    ensure_link(in[TOON_SKIN_SOCK_AO_ID]),
                    ensure_link(in[TOON_SKIN_SOCK_NORMAL_ID]),
                    ensure_link(in[TOON_SKIN_SOCK_WEIGHT_ID]),
                    ensure_link(in[TOON_SKIN_SOCK_DIFFUSE_WARP_ID]),
                    ramp_texture,
                    &out[0].link);
  }

  const bool use_transparency = in[TOON_SKIN_SOCK_ALPHA_ID].socket_not_one();
  const bool use_rim = in[TOON_SKIN_SOCK_RIM_WEIGHT_ID].socket_not_zero();

  eGPUMaterialFlag flag = GPU_MATFLAG_DIFFUSE;
  if (use_transparency) {
    flag |= GPU_MATFLAG_TRANSPARENT;
  }
  if (use_rim) {
    flag |= GPU_MATFLAG_EMISSION;
  }

  /* Toon diffuse stores its warp in an additional G-buffer layer. Marking reflection as maybe
   * colored prevents the two-closure simple layout from dropping that layer. */
  flag |= GPU_MATFLAG_REFLECTION_MAYBE_COLORED;

  GPU_material_flag_set(mat, flag);

  /* LightIndex is only used by the direct-light GPU function. Leaving it on the stack would
   * make GPU_stack_link() pass it as a node_bsdf_toon_skin argument and shift the extra
   * parameters (direct weight, precomputed diffuse color). */
  for (int i = 0; !in[i].end; i++) {
    if (i == TOON_SKIN_SOCK_LIGHT_INDEX_ID) {
      in[i].type = GPU_NONE;
      break;
    }
  }

  const float direct_weight = ramp_image ? 0.0f : 1.0f;
  return GPU_stack_link(mat,
                        node,
                        "node_bsdf_toon_skin",
                        in,
                        out,
                        GPU_constant(&direct_weight),
                        diffuse_color);
}

}  // namespace nodes::node_shader_bsdf_toon_skin_cc

void register_node_type_sh_bsdf_toon_skin()
{
  namespace file_ns = nodes::node_shader_bsdf_toon_skin_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfToonSkin"_ustr, SH_NODE_BSDF_TOON_SKIN);
  ntype.ui_name = "Toon Skin BSDF";
  ntype.ui_description =
      "Eevee toon skin shader using ramp lighting and a skin color LUT, without PBR layers";
  ntype.enum_name_legacy = "BSDF_TOON_SKIN";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_shader_init_toon_skin;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_toon_skin;
  bke::node_type_storage(
      ntype, "NodeShaderToonSkin", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
