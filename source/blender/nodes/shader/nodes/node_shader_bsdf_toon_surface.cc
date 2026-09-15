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

namespace nodes::node_shader_bsdf_toon_surface_cc {

enum ToonSurfaceMode {
  TOON_SURFACE_MODE_CLOSURE = 0,
  TOON_SURFACE_MODE_DIRECT_LIGHT = 1,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);

  b.use_custom_socket_order();

  b.add_output<decl::Shader>("BSDF"_ustr);

  b.add_input<decl::Color>("Base Color"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f})
      .description("Diffuse albedo; connect the diffuse texture here");
#define TOON_SURFACE_SOCK_BASE_COLOR_ID 0
  b.add_input<decl::Float>("Metallic"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Blend between dielectric and metallic surface models");
#define TOON_SURFACE_SOCK_METALLIC_ID 1
  b.add_input<decl::Float>("Roughness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Roughness of the GGX specular reflection");
#define TOON_SURFACE_SOCK_ROUGHNESS_ID 2
  b.add_input<decl::Float>("IOR"_ustr)
      .default_value(1.5f)
      .min(1.0f)
      .max(1000.0f)
      .description("Index of refraction used by the GGX specular layer");
#define TOON_SURFACE_SOCK_IOR_ID 3
  b.add_input<decl::Float>("Alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Surface opacity");
#define TOON_SURFACE_SOCK_ALPHA_ID 4
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
#define TOON_SURFACE_SOCK_NORMAL_ID 5
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);
#define TOON_SURFACE_SOCK_WEIGHT_ID 6

  PanelDeclarationBuilder &diffuse = b.add_panel("Diffuse"_ustr).default_closed(false);
  diffuse.add_input<decl::Float>("Diffuse Warp"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Wrap diffuse illumination around the surface; 0 is Lambert and 0.5 is Half-Lambert");
#define TOON_SURFACE_SOCK_DIFFUSE_WARP_ID 7
  diffuse.add_input<decl::Float>("AO"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Ambient occlusion applied to the diffuse component");
#define TOON_SURFACE_SOCK_AO_ID 8
  diffuse.add_input<decl::Float>("Diffuse LUT Influence"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("LUT Influence"_ustr)
      .description("Blend between the diffuse color and the color transformed by Diffuse LUT");
#define TOON_SURFACE_SOCK_DIFFUSE_LUT_INFLUENCE_ID 9
  diffuse.add_input<decl::Image>("Ramp Texture"_ustr)
      .description("Horizontal lighting ramp sampled using the warped normal-light angle");
#define TOON_SURFACE_SOCK_RAMP_TEXTURE_ID 10
  diffuse.add_input<decl::Image>("Diffuse LUT"_ustr)
      .description("Flattened 3D color LUT with dimensions N squared by N");
#define TOON_SURFACE_SOCK_DIFFUSE_LUT_ID 11

  PanelDeclarationBuilder &specular = b.add_panel("Specular"_ustr).default_closed(true);
  specular.add_input<decl::Float>("Specular IOR Level"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("IOR Level"_ustr)
      .description("Adjust the intensity of dielectric GGX reflection");
#define TOON_SURFACE_SOCK_SPECULAR_ID 12
  specular.add_input<decl::Color>("Specular Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Tint the GGX specular reflection");
#define TOON_SURFACE_SOCK_SPECULAR_TINT_ID 13

  PanelDeclarationBuilder &sheen = b.add_panel("Fibre / Sheen"_ustr).default_closed(true);
  sheen.add_input<decl::Float>("Sheen Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the microfiber sheen layer");
#define TOON_SURFACE_SOCK_SHEEN_WEIGHT_ID 14
  sheen.add_input<decl::Float>("Sheen Roughness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Roughness"_ustr)
      .description("Roughness of the microfiber sheen layer");
#define TOON_SURFACE_SOCK_SHEEN_ROUGHNESS_ID 15
  sheen.add_input<decl::Color>("Sheen Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Color of the microfiber sheen reflection");
#define TOON_SURFACE_SOCK_SHEEN_TINT_ID 16

  PanelDeclarationBuilder &coat = b.add_panel("Coat"_ustr).default_closed(true);
  coat.add_input<decl::Float>("Coat Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight"_ustr)
      .description("Intensity of the dielectric coat layer");
#define TOON_SURFACE_SOCK_COAT_WEIGHT_ID 17
  coat.add_input<decl::Float>("Coat Roughness"_ustr)
      .default_value(0.03f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Roughness"_ustr)
      .description("Roughness of the coat layer");
#define TOON_SURFACE_SOCK_COAT_ROUGHNESS_ID 18
  coat.add_input<decl::Float>("Coat IOR"_ustr)
      .default_value(1.5f)
      .min(1.0f)
      .max(4.0f)
      .short_label("IOR"_ustr)
      .description("Index of refraction of the coat layer");
#define TOON_SURFACE_SOCK_COAT_IOR_ID 19
  coat.add_input<decl::Color>("Coat Tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint"_ustr)
      .description("Absorption tint of the coat layer");
#define TOON_SURFACE_SOCK_COAT_TINT_ID 20
  coat.add_input<decl::Vector>("Coat Normal"_ustr).short_label("Normal"_ustr).hide_value();
#define TOON_SURFACE_SOCK_COAT_NORMAL_ID 21

  b.add_input<decl::Int>("LightIndex"_ustr).available(is_gpu_internal);
#define TOON_SURFACE_SOCK_LIGHT_INDEX_ID 22
}

static void node_shader_init_toon_surface(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderToonSurface *storage = MEM_new<NodeShaderToonSurface>(__func__);
  BKE_imageuser_default(&storage->ramp_iuser);
  BKE_imageuser_default(&storage->diffuse_lut_iuser);
  node->storage = storage;
  node->custom1 = TOON_SURFACE_MODE_CLOSURE;
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

static int node_shader_gpu_bsdf_toon_surface(GPUMaterial *mat,
                                             bNode *node,
                                             bNodeExecData * /*execdata*/,
                                             GPUNodeStack *in,
                                             GPUNodeStack *out)
{
  bNode *node_original = node->runtime->original ? node->runtime->original : node;
  NodeShaderToonSurface *storage_original = static_cast<NodeShaderToonSurface *>(
      node_original->storage);

  Image *ramp_image = image_from_socket(*node, "Ramp Texture"_ustr);
  Image *diffuse_lut_image = image_from_socket(*node, "Diffuse LUT"_ustr);

  if (!in[TOON_SURFACE_SOCK_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[TOON_SURFACE_SOCK_NORMAL_ID].link);
  }
  if (!in[TOON_SURFACE_SOCK_COAT_NORMAL_ID].link) {
    GPU_link(mat, "world_normals_get", &in[TOON_SURFACE_SOCK_COAT_NORMAL_ID].link);
  }

  GPUSamplerState lookup_sampler = GPUSamplerState::default_sampler();
  lookup_sampler.filtering = GPU_SAMPLER_FILTERING_LINEAR;
  lookup_sampler.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
  lookup_sampler.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;

  GPUNodeLink *diffuse_color = ensure_link(in[TOON_SURFACE_SOCK_BASE_COLOR_ID]);

  if (diffuse_lut_image) {
    GPUNodeLink *lut_color = nullptr;
    if (!GPU_link(mat,
                  "toon_surface_diffuse_lut",
                  diffuse_color,
                  ensure_link(in[TOON_SURFACE_SOCK_DIFFUSE_LUT_INFLUENCE_ID]),
                  GPU_image(mat,
                            diffuse_lut_image,
                            &storage_original->diffuse_lut_iuser,
                            lookup_sampler),
                  &lut_color))
    {
      return false;
    }
    diffuse_color = lut_color;
  }

  if (node->custom1 == TOON_SURFACE_MODE_DIRECT_LIGHT) {
    if (!ramp_image) {
      return false;
    }

    GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTING | GPU_MATFLAG_DIFFUSE);
    GPUNodeLink *ramp_texture = GPU_image(
        mat, ramp_image, &storage_original->ramp_iuser, lookup_sampler);
    return GPU_link(mat,
                    "node_bsdf_toon_surface_direct",
                    ensure_link(in[TOON_SURFACE_SOCK_LIGHT_INDEX_ID]),
                    diffuse_color,
                    ensure_link(in[TOON_SURFACE_SOCK_METALLIC_ID]),
                    ensure_link(in[TOON_SURFACE_SOCK_ALPHA_ID]),
                    ensure_link(in[TOON_SURFACE_SOCK_AO_ID]),
                    ensure_link(in[TOON_SURFACE_SOCK_NORMAL_ID]),
                    ensure_link(in[TOON_SURFACE_SOCK_WEIGHT_ID]),
                    ensure_link(in[TOON_SURFACE_SOCK_DIFFUSE_WARP_ID]),
                    ramp_texture,
                    &out[0].link);
  }

  const bool use_diffuse = in[TOON_SURFACE_SOCK_SHEEN_WEIGHT_ID].socket_not_zero() ||
                           in[TOON_SURFACE_SOCK_METALLIC_ID].socket_not_one();
  const bool use_coat = in[TOON_SURFACE_SOCK_COAT_WEIGHT_ID].socket_not_zero();
  const bool use_transparency = in[TOON_SURFACE_SOCK_ALPHA_ID].socket_not_one();

  eGPUMaterialFlag flag = GPU_MATFLAG_GLOSSY;
  if (use_diffuse) {
    flag |= GPU_MATFLAG_DIFFUSE;
  }
  if (use_coat) {
    flag |= GPU_MATFLAG_COAT;
  }
  if (use_transparency) {
    flag |= GPU_MATFLAG_TRANSPARENT;
  }

  /* Toon diffuse stores its warp in an additional G-buffer layer. Marking reflection as maybe
   * colored prevents the two-closure simple layout from dropping that layer. */
  flag |= GPU_MATFLAG_REFLECTION_MAYBE_COLORED;

  GPU_material_flag_set(mat, flag);

  const float zero = 0.0f;
  if (!use_coat && in[TOON_SURFACE_SOCK_COAT_WEIGHT_ID].link == nullptr) {
    in[TOON_SURFACE_SOCK_COAT_WEIGHT_ID].link = GPU_constant(&zero);
  }

  /* LightIndex is only used by the direct-light GPU function. Leaving it on the stack would
   * make GPU_stack_link() pass it as a node_bsdf_toon_surface argument and shift the extra
   * parameters (multiscatter, direct weight, precomputed diffuse color). */
  for (int i = 0; !in[i].end; i++) {
    if (i == TOON_SURFACE_SOCK_LIGHT_INDEX_ID) {
      in[i].type = GPU_NONE;
      break;
    }
  }

  const float use_multiscatter = 1.0f;
  const float direct_weight = ramp_image ? 0.0f : 1.0f;
  return GPU_stack_link(mat,
                        node,
                        "node_bsdf_toon_surface",
                        in,
                        out,
                        GPU_constant(&use_multiscatter),
                        GPU_constant(&direct_weight),
                        diffuse_color);
}

}  // namespace nodes::node_shader_bsdf_toon_surface_cc

void register_node_type_sh_bsdf_toon_surface()
{
  namespace file_ns = nodes::node_shader_bsdf_toon_surface_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfToonSurface"_ustr, SH_NODE_BSDF_TOON_SURFACE);
  ntype.ui_name = "Toon Surface BSDF";
  ntype.ui_description =
      "Eevee surface shader combining Half-Lambert diffuse with GGX specular, microfiber sheen, "
      "and coat layers";
  ntype.enum_name_legacy = "BSDF_TOON_SURFACE";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_shader_init_toon_surface;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_toon_surface;
  bke::node_type_storage(
      ntype, "NodeShaderToonSurface", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
