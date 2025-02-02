#include "config.h"

#include "gskvulkanrenderpassprivate.h"

#include "gskdebugprivate.h"
#include "gskprofilerprivate.h"
#include "gskrendernodeprivate.h"
#include "gskrenderer.h"
#include "gskrendererprivate.h"
#include "gskroundedrectprivate.h"
#include "gsktransform.h"
#include "gskvulkanblendmodepipelineprivate.h"
#include "gskvulkanblurpipelineprivate.h"
#include "gskvulkanborderpipelineprivate.h"
#include "gskvulkanboxshadowpipelineprivate.h"
#include "gskvulkanclipprivate.h"
#include "gskvulkancolorpipelineprivate.h"
#include "gskvulkancolortextpipelineprivate.h"
#include "gskvulkancrossfadepipelineprivate.h"
#include "gskvulkaneffectpipelineprivate.h"
#include "gskvulkanlineargradientpipelineprivate.h"
#include "gskvulkantextpipelineprivate.h"
#include "gskvulkantexturepipelineprivate.h"
#include "gskvulkanimageprivate.h"
#include "gskvulkanpushconstantsprivate.h"
#include "gskvulkanrendererprivate.h"
#include "gskprivate.h"

#include "gdk/gdkvulkancontextprivate.h"

#define ORTHO_NEAR_PLANE        -10000
#define ORTHO_FAR_PLANE          10000

typedef struct _GskVulkanParseState GskVulkanParseState;

typedef union _GskVulkanOp GskVulkanOp;
typedef struct _GskVulkanOpRender GskVulkanOpRender;
typedef struct _GskVulkanOpText GskVulkanOpText;
typedef struct _GskVulkanOpPushConstants GskVulkanOpPushConstants;
typedef struct _GskVulkanOpScissor GskVulkanOpScissor;

typedef enum {
  /* GskVulkanOpRender */
  GSK_VULKAN_OP_FALLBACK,
  GSK_VULKAN_OP_FALLBACK_CLIP,
  GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP,
  GSK_VULKAN_OP_TEXTURE,
  GSK_VULKAN_OP_TEXTURE_SCALE,
  GSK_VULKAN_OP_COLOR,
  GSK_VULKAN_OP_LINEAR_GRADIENT,
  GSK_VULKAN_OP_OPACITY,
  GSK_VULKAN_OP_BLUR,
  GSK_VULKAN_OP_COLOR_MATRIX,
  GSK_VULKAN_OP_BORDER,
  GSK_VULKAN_OP_INSET_SHADOW,
  GSK_VULKAN_OP_OUTSET_SHADOW,
  GSK_VULKAN_OP_REPEAT,
  GSK_VULKAN_OP_CROSS_FADE,
  GSK_VULKAN_OP_BLEND_MODE,
  /* GskVulkanOpText */
  GSK_VULKAN_OP_TEXT,
  GSK_VULKAN_OP_COLOR_TEXT,
  /* GskVulkanOpPushConstants */
  GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS,
  /* GskVulkanOpScissor */
  GSK_VULKAN_OP_SCISSOR,
} GskVulkanOpType;

/* render ops with 0, 1 or 2 sources */
struct _GskVulkanOpRender
{
  GskVulkanOpType      type;
  GskRenderNode       *node; /* node that's the source of this op */
  graphene_point_t     offset; /* offset of the node */
  GskVulkanPipeline   *pipeline; /* pipeline to use */
  GskRoundedRect       clip; /* clip rect (or random memory if not relevant) */
  GskVulkanImage      *source; /* source image to render */
  GskVulkanImage      *source2; /* second source image to render (if relevant) */
  gsize                vertex_offset; /* offset into vertex buffer */
  guint32              image_descriptor[2]; /* index into descriptor for the (image, sampler) */
  guint32              image_descriptor2[2]; /* index into descriptor for the 2nd image (if relevant) */
  gsize                buffer_offset; /* offset into buffer */
  graphene_rect_t      source_rect; /* area that source maps to */
  graphene_rect_t      source2_rect; /* area that source2 maps to */
};

struct _GskVulkanOpText
{
  GskVulkanOpType      type;
  GskRenderNode       *node; /* node that's the source of this op */
  graphene_point_t     offset; /* offset of the node */
  GskVulkanPipeline   *pipeline; /* pipeline to use */
  GskRoundedRect       clip; /* clip rect (or random memory if not relevant) */
  GskVulkanImage      *source; /* source image to render */
  gsize                vertex_offset; /* offset into vertex buffer */
  guint32              image_descriptor[2]; /* index into descriptor for the (image, sampler) */
  guint                texture_index; /* index of the texture in the glyph cache */
  guint                start_glyph; /* the first glyph in nodes glyphstring that we render */
  guint                num_glyphs; /* number of *non-empty* glyphs (== instances) we render */
  float                scale;
};

struct _GskVulkanOpPushConstants
{
  GskVulkanOpType         type;
  GskRenderNode          *node; /* node that's the source of this op */
  graphene_vec2_t         scale;
  graphene_matrix_t       mvp;
  GskRoundedRect          clip;
};

struct _GskVulkanOpScissor
{
  GskVulkanOpType         type;
  GskRenderNode          *node; /* node that's the source of this op */
  cairo_rectangle_int_t   rect;
};

union _GskVulkanOp
{
  GskVulkanOpType          type;
  GskVulkanOpRender        render;
  GskVulkanOpText          text;
  GskVulkanOpPushConstants constants;
  GskVulkanOpScissor       scissor;
};

struct _GskVulkanRenderPass
{
  GdkVulkanContext *vulkan;

  GArray *render_ops;

  GskVulkanImage *target;
  graphene_rect_t viewport;
  cairo_region_t *clip;

  graphene_vec2_t scale;

  VkRenderPass render_pass;
  VkFramebuffer framebuffer;
  VkSemaphore signal_semaphore;
  GArray *wait_semaphores;
  GskVulkanBuffer *vertex_data;
};

struct _GskVulkanParseState
{
  cairo_rectangle_int_t  scissor;
  graphene_point_t       offset;
  graphene_vec2_t        scale;
  GskTransform          *modelview;
  graphene_matrix_t      projection;
  GskVulkanClip          clip;
};

#ifdef G_ENABLE_DEBUG
static GQuark fallback_pixels_quark;
static GQuark texture_pixels_quark;
#endif

GskVulkanRenderPass *
gsk_vulkan_render_pass_new (GdkVulkanContext      *context,
                            GskVulkanImage        *target,
                            const graphene_vec2_t *scale,
                            const graphene_rect_t *viewport,
                            cairo_region_t        *clip,
                            VkSemaphore            signal_semaphore)
{
  GskVulkanRenderPass *self;
  VkImageLayout final_layout;

  self = g_new0 (GskVulkanRenderPass, 1);
  self->vulkan = g_object_ref (context);
  self->render_ops = g_array_new (FALSE, FALSE, sizeof (GskVulkanOp));

  self->target = g_object_ref (target);
  self->clip = cairo_region_copy (clip);
  self->viewport = *viewport;
  graphene_vec2_init_from_vec2 (&self->scale, scale);

  if (signal_semaphore != VK_NULL_HANDLE) // this is a dependent pass
    final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  else
    final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  GSK_VK_CHECK (vkCreateRenderPass, gdk_vulkan_context_get_device (self->vulkan),
                                    &(VkRenderPassCreateInfo) {
                                        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = (VkAttachmentDescription[]) {
                                           {
                                              .format = gsk_vulkan_image_get_vk_format (target),
                                              .samples = VK_SAMPLE_COUNT_1_BIT,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                              .finalLayout = final_layout
                                           }
                                        },
                                        .subpassCount = 1,
                                        .pSubpasses = (VkSubpassDescription []) {
                                           {
                                              .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              .inputAttachmentCount = 0,
                                              .colorAttachmentCount = 1,
                                              .pColorAttachments = (VkAttachmentReference []) {
                                                 {
                                                    .attachment = 0,
                                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                  }
                                               },
                                               .pResolveAttachments = (VkAttachmentReference []) {
                                                  {
                                                     .attachment = VK_ATTACHMENT_UNUSED,
                                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                  }
                                               },
                                               .pDepthStencilAttachment = NULL,
                                            }
                                         },
                                         .dependencyCount = 0
                                      },
                                      NULL,
                                      &self->render_pass);

  GSK_VK_CHECK (vkCreateFramebuffer, gdk_vulkan_context_get_device (self->vulkan),
                                     &(VkFramebufferCreateInfo) {
                                         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                         .renderPass = self->render_pass,
                                         .attachmentCount = 1,
                                         .pAttachments = (VkImageView[1]) {
                                             gsk_vulkan_image_get_image_view (target)
                                         },
                                         .width = gsk_vulkan_image_get_width (target),
                                         .height = gsk_vulkan_image_get_height (target),
                                         .layers = 1
                                     },
                                     NULL,
                                     &self->framebuffer);

  self->signal_semaphore = signal_semaphore;
  self->wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
  self->vertex_data = NULL;

#ifdef G_ENABLE_DEBUG
  if (fallback_pixels_quark == 0)
    {
      fallback_pixels_quark = g_quark_from_static_string ("fallback-pixels");
      texture_pixels_quark = g_quark_from_static_string ("texture-pixels");
    }
#endif

  return self;
}

void
gsk_vulkan_render_pass_free (GskVulkanRenderPass *self)
{
  VkDevice device = gdk_vulkan_context_get_device (self->vulkan);

  g_array_unref (self->render_ops);
  g_object_unref (self->vulkan);
  g_object_unref (self->target);
  cairo_region_destroy (self->clip);
  vkDestroyFramebuffer (device, self->framebuffer, NULL);
  vkDestroyRenderPass (device, self->render_pass, NULL);

  if (self->vertex_data)
    gsk_vulkan_buffer_free (self->vertex_data);
  if (self->signal_semaphore != VK_NULL_HANDLE)
    vkDestroySemaphore (device, self->signal_semaphore, NULL);
  g_array_unref (self->wait_semaphores);

  g_free (self);
}

static void
gsk_vulkan_render_pass_append_scissor (GskVulkanRenderPass       *self,
                                       GskRenderNode             *node,
                                       const GskVulkanParseState *state)
{
  GskVulkanOp op = {
    .scissor.type  = GSK_VULKAN_OP_SCISSOR,
    .scissor.node  = node,
    .scissor.rect  = state->scissor
  };
  g_array_append_val (self->render_ops, op);
}

static void
gsk_vulkan_render_pass_append_push_constants (GskVulkanRenderPass       *self,
                                              GskRenderNode             *node,
                                              const GskVulkanParseState *state)
{
  GskVulkanOp op = {
    .constants.type  = GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS,
    .constants.node  = node,
    .constants.scale = state->scale,
    .constants.clip  = state->clip.rect,
  };

  if (state->modelview)
    {
      gsk_transform_to_matrix (state->modelview, &op.constants.mvp);
      graphene_matrix_multiply (&op.constants.mvp, &state->projection, &op.constants.mvp);
    }
  else
    graphene_matrix_init_from_matrix (&op.constants.mvp, &state->projection);

  g_array_append_val (self->render_ops, op);
}

#define FALLBACK(...) G_STMT_START { \
  GSK_RENDERER_DEBUG (gsk_vulkan_render_get_renderer (render), FALLBACK, __VA_ARGS__); \
  return FALSE; \
}G_STMT_END

static GskVulkanPipeline *
gsk_vulkan_render_pass_get_pipeline (GskVulkanRenderPass   *self,
                                     GskVulkanRender       *render,
                                     GskVulkanPipelineType  pipeline_type)
{
  return gsk_vulkan_render_get_pipeline (render,
                                         pipeline_type,
                                         self->render_pass);
}

static void
gsk_vulkan_render_pass_add_node (GskVulkanRenderPass       *self,
                                 GskVulkanRender           *render,
                                 const GskVulkanParseState *state,
                                 GskRenderNode             *node);

static inline gboolean
gsk_vulkan_render_pass_add_fallback_node (GskVulkanRenderPass       *self,
                                          GskVulkanRender           *render,
                                          const GskVulkanParseState *state,
                                          GskRenderNode             *node)
{
  GskVulkanOp op = {
    .render.node = node,
    .render.offset = state->offset,
  };

  switch (state->clip.type)
    {
      case GSK_VULKAN_CLIP_NONE:
        op.type = GSK_VULKAN_OP_FALLBACK;
        break;
      case GSK_VULKAN_CLIP_RECT:
        op.type = GSK_VULKAN_OP_FALLBACK_CLIP;
        gsk_rounded_rect_init_copy (&op.render.clip, &state->clip.rect);
        break;
      case GSK_VULKAN_CLIP_ROUNDED:
        op.type = GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP;
        gsk_rounded_rect_init_copy (&op.render.clip, &state->clip.rect);
        break;
      case GSK_VULKAN_CLIP_ALL_CLIPPED:
      default:
        g_assert_not_reached ();
        return FALSE;
    }

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, GSK_VULKAN_PIPELINE_TEXTURE);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_implode (GskVulkanRenderPass       *self,
                                GskVulkanRender           *render,
                                const GskVulkanParseState *state,
                                GskRenderNode             *node)
{
  g_assert_not_reached ();
  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_container_node (GskVulkanRenderPass       *self,
                                           GskVulkanRender           *render,
                                           const GskVulkanParseState *state,
                                           GskRenderNode             *node)
{
  for (guint i = 0; i < gsk_container_node_get_n_children (node); i++)
    gsk_vulkan_render_pass_add_node (self, render, state, gsk_container_node_get_child (node, i));

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_cairo_node (GskVulkanRenderPass       *self,
                                       GskVulkanRender           *render,
                                       const GskVulkanParseState *state,
                                       GskRenderNode             *node)
{
  /* We're using recording surfaces, so drawing them to an image
   * surface and uploading them is the right thing.
   * But that's exactly what the fallback code does.
   */
  if (gsk_cairo_node_get_surface (node) != NULL)
    return gsk_vulkan_render_pass_add_fallback_node (self, render, state, node);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_color_node (GskVulkanRenderPass       *self,
                                       GskVulkanRender           *render,
                                       const GskVulkanParseState *state,
                                       GskRenderNode             *node)
{
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_COLOR,
    .render.node = node,
    .render.offset = state->offset,
  };
  GskVulkanPipelineType pipeline_type;

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_linear_gradient_node (GskVulkanRenderPass       *self,
                                                 GskVulkanRender           *render,
                                                 const GskVulkanParseState *state,
                                                 GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_LINEAR_GRADIENT,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_LINEAR_GRADIENT;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_LINEAR_GRADIENT_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_LINEAR_GRADIENT_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_border_node (GskVulkanRenderPass       *self,
                                        GskVulkanRender           *render,
                                        const GskVulkanParseState *state,
                                        GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_BORDER,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_BORDER;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_BORDER_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_BORDER_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_texture_node (GskVulkanRenderPass       *self,
                                         GskVulkanRender           *render,
                                         const GskVulkanParseState *state,
                                         GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_TEXTURE,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_texture_scale_node (GskVulkanRenderPass       *self,
                                               GskVulkanRender           *render,
                                               const GskVulkanParseState *state,
                                               GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_TEXTURE_SCALE,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_inset_shadow_node (GskVulkanRenderPass       *self,
                                              GskVulkanRender           *render,
                                              const GskVulkanParseState *state,
                                              GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_INSET_SHADOW,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_inset_shadow_node_get_blur_radius (node) > 0)
    FALLBACK ("Blur support not implemented for inset shadows");
  else if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_INSET_SHADOW;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_INSET_SHADOW_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_INSET_SHADOW_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_outset_shadow_node (GskVulkanRenderPass       *self,
                                               GskVulkanRender           *render,
                                               const GskVulkanParseState *state,
                                               GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_OUTSET_SHADOW,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_outset_shadow_node_get_blur_radius (node) > 0)
    FALLBACK ("Blur support not implemented for outset shadows");
  else if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_OUTSET_SHADOW;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_OUTSET_SHADOW_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_OUTSET_SHADOW_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_transform_node (GskVulkanRenderPass       *self,
                                           GskVulkanRender           *render,
                                           const GskVulkanParseState *state,
                                           GskRenderNode             *node)
{
  GskRenderNode *child;
  GskTransform *transform;
  GskVulkanParseState new_state;

  child = gsk_transform_node_get_child (node);
  transform = gsk_transform_node_get_transform (node);

  switch (gsk_transform_get_category (transform))
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      {
        float dx, dy;
        gsk_transform_to_translate (transform, &dx, &dy);
        new_state = *state;
        new_state.offset.x += dx;
        new_state.offset.y += dy;
        gsk_vulkan_render_pass_add_node (self, render, &new_state, child);
      }
      return TRUE;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      {
        float dx, dy, scale_x, scale_y;
        gsk_transform_to_affine (transform, &scale_x, &scale_y, &dx, &dy);
        gsk_vulkan_clip_scale (&new_state.clip, &state->clip, scale_x, scale_y);
        new_state.offset.x = (state->offset.x + dx) / scale_x;
        new_state.offset.y = (state->offset.y + dy) / scale_y;
        graphene_vec2_init (&new_state.scale, fabs (scale_x), fabs (scale_y));
        graphene_vec2_multiply (&new_state.scale, &state->scale, &new_state.scale);
        new_state.modelview = gsk_transform_scale (gsk_transform_ref (state->modelview),
                                                   scale_x / fabs (scale_x),
                                                   scale_y / fabs (scale_y));
      }
      break;

    case GSK_TRANSFORM_CATEGORY_2D:
      {
        float skew_x, skew_y, scale_x, scale_y, angle, dx, dy;
        GskTransform *clip_transform;

        clip_transform = gsk_transform_transform (gsk_transform_translate (NULL, &state->offset), transform);

        if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
          {
            gsk_vulkan_clip_init_empty (&new_state.clip, &child->bounds);
          }
        else if (!gsk_vulkan_clip_transform (&new_state.clip, &state->clip, clip_transform, &child->bounds))
          {
            gsk_transform_unref (clip_transform);
            FALLBACK ("Transform nodes can't deal with clip type %u", state->clip.type);
          }

        new_state.modelview = gsk_transform_ref (state->modelview);
        new_state.modelview = gsk_transform_scale (state->modelview,
                                                   graphene_vec2_get_x (&state->scale),
                                                   graphene_vec2_get_y (&state->scale));
        new_state.modelview = gsk_transform_transform (new_state.modelview, clip_transform);
        gsk_transform_unref (clip_transform);

        gsk_transform_to_2d_components (new_state.modelview,
                                        &skew_x, &skew_y,
                                        &scale_x, &scale_y,
                                        &angle,
                                        &dx, &dy);
        scale_x = fabs (scale_x);
        scale_y = fabs (scale_y);
        new_state.modelview = gsk_transform_scale (new_state.modelview, 1 / scale_x, 1 / scale_y);
        graphene_vec2_init (&new_state.scale, scale_x, scale_y);
        new_state.offset = *graphene_point_zero ();
      }
      break;

    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
      {
        graphene_quaternion_t rotation;
        graphene_matrix_t matrix;
        graphene_vec4_t perspective;
        graphene_vec3_t translation;
        graphene_vec3_t matrix_scale;
        graphene_vec3_t shear;
        GskTransform *clip_transform;
        float scale_x, scale_y, old_pixels, new_pixels;

        clip_transform = gsk_transform_transform (gsk_transform_translate (NULL, &state->offset), transform);

        if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
          {
            gsk_vulkan_clip_init_empty (&new_state.clip, &child->bounds);
          }
        else if (!gsk_vulkan_clip_transform (&new_state.clip, &state->clip, clip_transform, &child->bounds))
          {
            gsk_transform_unref (clip_transform);
            FALLBACK ("Transform nodes can't deal with clip type %u", state->clip.type);
          }

        new_state.modelview = gsk_transform_ref (state->modelview);
        new_state.modelview = gsk_transform_scale (state->modelview,
                                                   graphene_vec2_get_x (&state->scale),
                                                   graphene_vec2_get_y (&state->scale));                                                  
        new_state.modelview = gsk_transform_transform (new_state.modelview, clip_transform);
        gsk_transform_unref (clip_transform);

        gsk_transform_to_matrix (new_state.modelview, &matrix);
        graphene_matrix_decompose (&matrix,
                                   &translation,
                                   &matrix_scale,
                                   &rotation,
                                   &shear,
                                   &perspective);

        scale_x = fabs (graphene_vec3_get_x (&matrix_scale));
        scale_y = fabs (graphene_vec3_get_y (&matrix_scale));
        old_pixels = graphene_vec2_get_x (&state->scale) * graphene_vec2_get_y (&state->scale) *
                     state->clip.rect.bounds.size.width * state->clip.rect.bounds.size.height;
        new_pixels = scale_x * scale_y * new_state.clip.rect.bounds.size.width * new_state.clip.rect.bounds.size.height;
        if (new_pixels > 2 * old_pixels)
          {
            float forced_downscale = 2 * old_pixels / new_pixels;
            scale_x *= forced_downscale;
            scale_y *= forced_downscale;
          }
        new_state.modelview = gsk_transform_scale (new_state.modelview, 1 / scale_x, 1 / scale_y);
        graphene_vec2_init (&new_state.scale, scale_x, scale_y);
        new_state.offset = *graphene_point_zero ();
      }
      break;

    default:
      break;
    }

  new_state.scissor = state->scissor;
  graphene_matrix_init_from_matrix (&new_state.projection, &state->projection);

  gsk_vulkan_render_pass_append_push_constants (self, node, &new_state);

  gsk_vulkan_render_pass_add_node (self, render, &new_state, child);

  gsk_vulkan_render_pass_append_push_constants (self, node, state);

  gsk_transform_unref (new_state.modelview);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_opacity_node (GskVulkanRenderPass       *self,
                                         GskVulkanRender           *render,
                                         const GskVulkanParseState *state,
                                         GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_OPACITY,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_color_matrix_node (GskVulkanRenderPass       *self,
                                              GskVulkanRender           *render,
                                              const GskVulkanParseState *state,
                                              GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_COLOR_MATRIX,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static gboolean
clip_can_be_scissored (const graphene_rect_t *rect,
                       const graphene_vec2_t *scale,
                       GskTransform          *modelview,
                       cairo_rectangle_int_t *int_rect)
{
  graphene_rect_t transformed_rect;
  float scale_x = graphene_vec2_get_x (scale);
  float scale_y = graphene_vec2_get_y (scale);

  switch (gsk_transform_get_category (modelview))
    {
    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_2D:
      return FALSE;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      gsk_transform_transform_bounds (modelview, rect, &transformed_rect);
      rect = &transformed_rect;
      break;

    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    default:
      break;
    } 
  int_rect->x = rect->origin.x * scale_x;
  int_rect->y = rect->origin.y * scale_y;
  int_rect->width = rect->size.width * scale_x;
  int_rect->height = rect->size.height * scale_y;

  return int_rect->x == rect->origin.x * scale_x
      && int_rect->y == rect->origin.y * scale_y
      && int_rect->width == rect->size.width * scale_x
      && int_rect->height == rect->size.height * scale_y;
}

static inline gboolean
gsk_vulkan_render_pass_add_clip_node (GskVulkanRenderPass       *self,
                                      GskVulkanRender           *render,
                                      const GskVulkanParseState *state,
                                      GskRenderNode             *node)
{
  GskVulkanParseState new_state;
  graphene_rect_t clip;
  gboolean do_push_constants, do_scissor;

  graphene_rect_offset_r (gsk_clip_node_get_clip (node),
                          state->offset.x, state->offset.y,
                          &clip);

  /* Check if we can use scissoring for the clip */
  if (clip_can_be_scissored (&clip, &state->scale, state->modelview, &new_state.scissor))
    {
      if (!gdk_rectangle_intersect (&new_state.scissor, &state->scissor, &new_state.scissor))
        return TRUE;

      if (gsk_vulkan_clip_intersect_rect (&new_state.clip, &state->clip, &clip))
        {
          if (new_state.clip.type == GSK_VULKAN_CLIP_RECT)
            new_state.clip.type = GSK_VULKAN_CLIP_NONE;

          do_push_constants = TRUE;
        }
      else
        {
          gsk_vulkan_clip_init_copy (&new_state.clip, &state->clip);
          do_push_constants = FALSE;
        }
      
      do_scissor = TRUE;
    }
  else
    {
      if (!gsk_vulkan_clip_intersect_rect (&new_state.clip, &state->clip, &clip))
        FALLBACK ("Failed to find intersection between clip of type %u and rectangle", state->clip.type);

      new_state.scissor = state->scissor;

      do_push_constants = TRUE;
      do_scissor = FALSE;
    }

  if (new_state.clip.type == GSK_VULKAN_CLIP_ALL_CLIPPED)
    return TRUE;

  new_state.offset = state->offset;
  graphene_vec2_init_from_vec2 (&new_state.scale, &state->scale);
  new_state.modelview = state->modelview;
  graphene_matrix_init_from_matrix (&new_state.projection, &state->projection);

  if (do_scissor)
    gsk_vulkan_render_pass_append_scissor (self, node, &new_state);
  if (do_push_constants)
    gsk_vulkan_render_pass_append_push_constants (self, node, &new_state);

  gsk_vulkan_render_pass_add_node (self, render, &new_state, gsk_clip_node_get_child (node));

  if (do_push_constants)
    gsk_vulkan_render_pass_append_push_constants (self, node, state);
  if (do_scissor)
    gsk_vulkan_render_pass_append_scissor (self, node, state);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_rounded_clip_node (GskVulkanRenderPass       *self,
                                              GskVulkanRender           *render,
                                              const GskVulkanParseState *state,
                                              GskRenderNode             *node)
{
  GskVulkanParseState new_state;
  GskRoundedRect clip;

  clip = *gsk_rounded_clip_node_get_clip (node);
  gsk_rounded_rect_offset (&clip, state->offset.x, state->offset.y);

  if (!gsk_vulkan_clip_intersect_rounded_rect (&new_state.clip, &state->clip, &clip))
    FALLBACK ("Failed to find intersection between clip of type %u and rounded rectangle", state->clip.type);

  if (new_state.clip.type == GSK_VULKAN_CLIP_ALL_CLIPPED)
    return TRUE;

  new_state.scissor = state->scissor;
  new_state.offset = state->offset;
  graphene_vec2_init_from_vec2 (&new_state.scale, &state->scale);
  new_state.modelview = state->modelview;
  graphene_matrix_init_from_matrix (&new_state.projection, &state->projection);

  gsk_vulkan_render_pass_append_push_constants (self, node, &new_state);

  gsk_vulkan_render_pass_add_node (self, render, &new_state, gsk_rounded_clip_node_get_child (node));

  gsk_vulkan_render_pass_append_push_constants (self, node, state);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_repeat_node (GskVulkanRenderPass       *self,
                                        GskVulkanRender           *render,
                                        const GskVulkanParseState *state,
                                        GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_REPEAT,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (graphene_rect_get_area (gsk_repeat_node_get_child_bounds (node)) == 0)
    return TRUE;

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_TEXTURE_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_blend_node (GskVulkanRenderPass       *self,
                                       GskVulkanRender           *render,
                                       const GskVulkanParseState *state,
                                       GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_BLEND_MODE,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_BLEND_MODE;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_BLEND_MODE_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_BLEND_MODE_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_cross_fade_node (GskVulkanRenderPass       *self,
                                            GskVulkanRender           *render,
                                            const GskVulkanParseState *state,
                                            GskRenderNode             *node)
{
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_CROSS_FADE,
    .render.node = node,
    .render.offset = state->offset,
  };
  GskVulkanPipelineType pipeline_type;

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_CROSS_FADE;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_CROSS_FADE_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_CROSS_FADE_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_text_node (GskVulkanRenderPass       *self,
                                      GskVulkanRender           *render,
                                      const GskVulkanParseState *state,
                                      GskRenderNode             *node)
{
  GskVulkanOp op = {
    .render.node = node,
    .render.offset = state->offset,
  };
  GskVulkanPipelineType pipeline_type;
  const PangoGlyphInfo *glyphs;
  GskVulkanRenderer *renderer;
  const PangoFont *font;
  guint texture_index;
  guint num_glyphs;
  guint count;
  int x_position;
  int i;

  renderer = GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render));
  num_glyphs = gsk_text_node_get_num_glyphs (node);
  glyphs = gsk_text_node_get_glyphs (node, NULL);
  font = gsk_text_node_get_font (node);

  if (gsk_text_node_has_color_glyphs (node))
    {
      if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
        pipeline_type = GSK_VULKAN_PIPELINE_COLOR_TEXT;
      else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
        pipeline_type = GSK_VULKAN_PIPELINE_COLOR_TEXT_CLIP;
      else
        pipeline_type = GSK_VULKAN_PIPELINE_COLOR_TEXT_CLIP_ROUNDED;
      op.type = GSK_VULKAN_OP_COLOR_TEXT;
    }
  else
    {
      if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
        pipeline_type = GSK_VULKAN_PIPELINE_TEXT;
      else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
        pipeline_type = GSK_VULKAN_PIPELINE_TEXT_CLIP;
      else
        pipeline_type = GSK_VULKAN_PIPELINE_TEXT_CLIP_ROUNDED;
      op.type = GSK_VULKAN_OP_TEXT;
    }
  op.text.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);

  op.text.start_glyph = 0;
  op.text.texture_index = G_MAXUINT;
  op.text.scale = MAX (graphene_vec2_get_x (&state->scale), graphene_vec2_get_y (&state->scale));

  x_position = 0;
  for (i = 0, count = 0; i < num_glyphs; i++)
    {
      const PangoGlyphInfo *gi = &glyphs[i];

      texture_index = gsk_vulkan_renderer_cache_glyph (renderer,
                                                       (PangoFont *)font,
                                                       gi->glyph,
                                                       x_position + gi->geometry.x_offset,
                                                       gi->geometry.y_offset,
                                                       op.text.scale);
      if (op.text.texture_index == G_MAXUINT)
        op.text.texture_index = texture_index;
      if (texture_index != op.text.texture_index)
        {
          op.text.num_glyphs = count;

          g_array_append_val (self->render_ops, op);

          count = 1;
          op.text.start_glyph = i;
          op.text.texture_index = texture_index;
        }
      else
        count++;

      x_position += gi->geometry.width;
    }

  if (op.text.texture_index != G_MAXUINT && count != 0)
    {
      op.text.num_glyphs = count;
      g_array_append_val (self->render_ops, op);
    }

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_blur_node (GskVulkanRenderPass       *self,
                                      GskVulkanRender           *render,
                                      const GskVulkanParseState *state,
                                      GskRenderNode             *node)
{
  GskVulkanPipelineType pipeline_type;
  GskVulkanOp op = {
    .render.type = GSK_VULKAN_OP_BLUR,
    .render.node = node,
    .render.offset = state->offset,
  };

  if (gsk_vulkan_clip_contains_rect (&state->clip, &state->offset, &node->bounds))
    pipeline_type = GSK_VULKAN_PIPELINE_BLUR;
  else if (state->clip.type == GSK_VULKAN_CLIP_RECT)
    pipeline_type = GSK_VULKAN_PIPELINE_BLUR_CLIP;
  else
    pipeline_type = GSK_VULKAN_PIPELINE_BLUR_CLIP_ROUNDED;

  op.render.pipeline = gsk_vulkan_render_pass_get_pipeline (self, render, pipeline_type);
  g_array_append_val (self->render_ops, op);

  return TRUE;
}

static inline gboolean
gsk_vulkan_render_pass_add_debug_node (GskVulkanRenderPass       *self,
                                       GskVulkanRender           *render,
                                       const GskVulkanParseState *state,
                                       GskRenderNode             *node)
{
  gsk_vulkan_render_pass_add_node (self, render, state, gsk_debug_node_get_child (node));
  return TRUE;
}

#undef FALLBACK

typedef gboolean (*GskVulkanRenderPassNodeFunc) (GskVulkanRenderPass       *self,
                                                 GskVulkanRender           *render,
                                                 const GskVulkanParseState *state,
                                                 GskRenderNode             *node);

/* TODO: implement remaining nodes */
static const GskVulkanRenderPassNodeFunc nodes_vtable[] = {
  [GSK_NOT_A_RENDER_NODE] = gsk_vulkan_render_pass_implode,
  [GSK_CONTAINER_NODE] = gsk_vulkan_render_pass_add_container_node,
  [GSK_CAIRO_NODE] = gsk_vulkan_render_pass_add_cairo_node,
  [GSK_COLOR_NODE] = gsk_vulkan_render_pass_add_color_node,
  [GSK_LINEAR_GRADIENT_NODE] = gsk_vulkan_render_pass_add_linear_gradient_node,
  [GSK_REPEATING_LINEAR_GRADIENT_NODE] = gsk_vulkan_render_pass_add_linear_gradient_node,
  [GSK_RADIAL_GRADIENT_NODE] = NULL,
  [GSK_REPEATING_RADIAL_GRADIENT_NODE] = NULL,
  [GSK_CONIC_GRADIENT_NODE] = NULL,
  [GSK_BORDER_NODE] = gsk_vulkan_render_pass_add_border_node,
  [GSK_TEXTURE_NODE] = gsk_vulkan_render_pass_add_texture_node,
  [GSK_INSET_SHADOW_NODE] = gsk_vulkan_render_pass_add_inset_shadow_node,
  [GSK_OUTSET_SHADOW_NODE] = gsk_vulkan_render_pass_add_outset_shadow_node,
  [GSK_TRANSFORM_NODE] = gsk_vulkan_render_pass_add_transform_node,
  [GSK_OPACITY_NODE] = gsk_vulkan_render_pass_add_opacity_node,
  [GSK_COLOR_MATRIX_NODE] = gsk_vulkan_render_pass_add_color_matrix_node,
  [GSK_REPEAT_NODE] = gsk_vulkan_render_pass_add_repeat_node,
  [GSK_CLIP_NODE] = gsk_vulkan_render_pass_add_clip_node,
  [GSK_ROUNDED_CLIP_NODE] = gsk_vulkan_render_pass_add_rounded_clip_node,
  [GSK_SHADOW_NODE] = NULL,
  [GSK_BLEND_NODE] = gsk_vulkan_render_pass_add_blend_node,
  [GSK_CROSS_FADE_NODE] = gsk_vulkan_render_pass_add_cross_fade_node,
  [GSK_TEXT_NODE] = gsk_vulkan_render_pass_add_text_node,
  [GSK_BLUR_NODE] = gsk_vulkan_render_pass_add_blur_node,
  [GSK_DEBUG_NODE] = gsk_vulkan_render_pass_add_debug_node,
  [GSK_GL_SHADER_NODE] = NULL,
  [GSK_TEXTURE_SCALE_NODE] = gsk_vulkan_render_pass_add_texture_scale_node,
  [GSK_MASK_NODE] = NULL,
};

static void
gsk_vulkan_render_pass_add_node (GskVulkanRenderPass       *self,
                                 GskVulkanRender           *render,
                                 const GskVulkanParseState *state,
                                 GskRenderNode             *node)
{
  GskVulkanRenderPassNodeFunc node_func;
  GskRenderNodeType node_type;
  gboolean fallback = FALSE;

  /* This catches the corner cases of empty nodes, so after this check
   * there's quaranteed to be at least 1 pixel that needs to be drawn */
  if (!gsk_vulkan_clip_may_intersect_rect (&state->clip, &state->offset, &node->bounds))
    return;

  node_type = gsk_render_node_get_node_type (node);
  if (node_type < G_N_ELEMENTS (nodes_vtable))
    node_func = nodes_vtable[node_type];
  else
    node_func = NULL;

  if (node_func)
    {
      if (!node_func (self, render, state, node))
        fallback = TRUE;
    }
  else
    {
      GSK_RENDERER_DEBUG (gsk_vulkan_render_get_renderer (render),
                          FALLBACK, "Unsupported node '%s'",
                          g_type_name_from_instance ((GTypeInstance *) node));
      fallback = TRUE;
    }

  if (fallback)
    gsk_vulkan_render_pass_add_fallback_node (self, render, state, node);
}

void
gsk_vulkan_render_pass_add (GskVulkanRenderPass *self,
                            GskVulkanRender     *render,
                            GskRenderNode       *node)
{
  GskVulkanParseState state;
  graphene_rect_t clip;
  float scale_x, scale_y;

  scale_x = 1 / graphene_vec2_get_x (&self->scale);
  scale_y = 1 / graphene_vec2_get_y (&self->scale);
  cairo_region_get_extents (self->clip, &state.scissor);
  clip = GRAPHENE_RECT_INIT(state.scissor.x, state.scissor.y,
                            state.scissor.width, state.scissor.height);
  graphene_rect_scale (&clip, scale_x, scale_y, &clip);
  gsk_vulkan_clip_init_empty (&state.clip, &clip);

  state.modelview = NULL;
  graphene_matrix_init_ortho (&state.projection,
                              0, self->viewport.size.width,
                              0, self->viewport.size.height,
                              2 * ORTHO_NEAR_PLANE - ORTHO_FAR_PLANE,
                              ORTHO_FAR_PLANE);
  graphene_vec2_init_from_vec2 (&state.scale, &self->scale);
  state.offset = GRAPHENE_POINT_INIT (-self->viewport.origin.x * scale_x,
                                      -self->viewport.origin.y * scale_y);

  gsk_vulkan_render_pass_append_scissor (self, node, &state);
  gsk_vulkan_render_pass_append_push_constants (self, node, &state);

  gsk_vulkan_render_pass_add_node (self, render, &state, node);
}

static GskVulkanImage *
gsk_vulkan_render_pass_render_offscreen (GdkVulkanContext      *vulkan,
                                         GskVulkanRender       *render,
                                         GskVulkanUploader     *uploader,
                                         VkSemaphore            semaphore,
                                         GskRenderNode         *node,
                                         const graphene_vec2_t *scale,
                                         const graphene_rect_t *viewport)
{
  graphene_rect_t view;
  cairo_region_t *clip;
  GskVulkanRenderPass *pass;
  GskVulkanImage *result;
  float scale_x, scale_y;

  scale_x = graphene_vec2_get_x (scale);
  scale_y = graphene_vec2_get_y (scale);
  view = GRAPHENE_RECT_INIT (scale_x * viewport->origin.x,
                             scale_y * viewport->origin.y,
                             ceil (scale_x * viewport->size.width),
                             ceil (scale_y * viewport->size.height));

  result = gsk_vulkan_image_new_for_offscreen (vulkan,
                                               gdk_vulkan_context_get_offscreen_format (vulkan,
                                                   gsk_render_node_get_preferred_depth (node)),
                                               view.size.width, view.size.height);

#ifdef G_ENABLE_DEBUG
  {
    GskProfiler *profiler = gsk_renderer_get_profiler (gsk_vulkan_render_get_renderer (render));
    gsk_profiler_counter_add (profiler,
                              texture_pixels_quark,
                              view.size.width * view.size.height);
  }
#endif

  clip = cairo_region_create_rectangle (&(cairo_rectangle_int_t) {
                                          0, 0,
                                          gsk_vulkan_image_get_width (result),
                                          gsk_vulkan_image_get_height (result)
                                        });

  pass = gsk_vulkan_render_pass_new (vulkan,
                                     result,
                                     scale,
                                     &view,
                                     clip,
                                     semaphore);

  cairo_region_destroy (clip);

  gsk_vulkan_render_add_render_pass (render, pass);
  gsk_vulkan_render_pass_add (pass, render, node);
  gsk_vulkan_render_add_cleanup_image (render, result);

  return result;
}

static GskVulkanImage *
gsk_vulkan_render_pass_get_node_as_texture (GskVulkanRenderPass    *self,
                                            GskVulkanRender        *render,
                                            GskVulkanUploader      *uploader,
                                            GskRenderNode          *node,
                                            const graphene_vec2_t  *scale,
                                            const graphene_rect_t  *clip_bounds,
                                            const graphene_point_t *clip_offset,
                                            graphene_rect_t        *tex_bounds)
{
  VkSemaphore semaphore;
  GskVulkanImage *result;
  GskVulkanImageMap map;
  gsize width, height;
  cairo_surface_t *surface;
  cairo_t *cr;

  switch ((guint) gsk_render_node_get_node_type (node))
    {
    case GSK_TEXTURE_NODE:
      result = gsk_vulkan_renderer_ref_texture_image (GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                      gsk_texture_node_get_texture (node),
                                                      uploader);
      gsk_vulkan_render_add_cleanup_image (render, result);
      *tex_bounds = node->bounds;
      return result;

    case GSK_CAIRO_NODE:
      /* We're using recording surfaces, so drawing them to an image
       * surface and uploading them is the right thing.
       * But that's exactly what the fallback code does.
       */
      break;

    default:
      {
        graphene_rect_t clipped;

        graphene_rect_offset_r (clip_bounds, - clip_offset->x, - clip_offset->y, &clipped);
        graphene_rect_intersection (&clipped, &node->bounds, &clipped);

        if (clipped.size.width == 0 || clipped.size.height == 0)
          return NULL;

        /* assuming the unclipped bounds should go to texture coordinates 0..1,
         * calculate the coordinates for the clipped texture size
         */
        *tex_bounds = clipped;

        vkCreateSemaphore (gdk_vulkan_context_get_device (self->vulkan),
                           &(VkSemaphoreCreateInfo) {
                             VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                             NULL,
                             0
                           },
                           NULL,
                           &semaphore);

        g_array_append_val (self->wait_semaphores, semaphore);

        return gsk_vulkan_render_pass_render_offscreen (self->vulkan,
                                                        render,
                                                        uploader,
                                                        semaphore,
                                                        node,
                                                        scale,
                                                        &clipped);
      }
   }

  GSK_RENDERER_DEBUG (gsk_vulkan_render_get_renderer (render), FALLBACK, "Node as texture not implemented for this case. Using %gx%g fallback surface",
                      ceil (node->bounds.size.width),
                      ceil (node->bounds.size.height));
#ifdef G_ENABLE_DEBUG
  {
    GskProfiler *profiler = gsk_renderer_get_profiler (gsk_vulkan_render_get_renderer (render));
    gsk_profiler_counter_add (profiler,
                              fallback_pixels_quark,
                              ceil (node->bounds.size.width) * ceil (node->bounds.size.height));
  }
#endif

  /* XXX: We could intersect bounds with clip bounds here */
  width = ceil (node->bounds.size.width * graphene_vec2_get_x (scale));
  height = ceil (node->bounds.size.height * graphene_vec2_get_y (scale));

  result = gsk_vulkan_image_new_for_upload (uploader, GDK_MEMORY_DEFAULT, width, height);
  gsk_vulkan_image_map_memory (result, uploader, GSK_VULKAN_WRITE, &map);
  surface = cairo_image_surface_create_for_data (map.data,
                                                 CAIRO_FORMAT_ARGB32,
                                                 width, height,
                                                 map.stride);
  cairo_surface_set_device_scale (surface,
                                  width / node->bounds.size.width,
                                  height / node->bounds.size.height);
  cr = cairo_create (surface);
  cairo_translate (cr, -node->bounds.origin.x, -node->bounds.origin.y);

  gsk_render_node_draw (node, cr);

  cairo_destroy (cr);

  cairo_surface_finish (surface);
  cairo_surface_destroy (surface);

  gsk_vulkan_image_unmap_memory (result, uploader, &map);
  gsk_vulkan_render_add_cleanup_image (render, result);

  *tex_bounds = node->bounds;

  return result;
}

static void
gsk_vulkan_render_pass_upload_fallback (GskVulkanRenderPass  *self,
                                        GskVulkanOpRender    *op,
                                        GskVulkanRender      *render,
                                        GskVulkanUploader    *uploader)
{
  GskRenderNode *node;
  GskVulkanImageMap map;
  gsize width, height;
  cairo_surface_t *surface;
  cairo_t *cr;

  node = op->node;

  GSK_RENDERER_DEBUG (gsk_vulkan_render_get_renderer (render), FALLBACK,
                      "Upload op=%s, node %s[%p], bounds %gx%g",
                      op->type == GSK_VULKAN_OP_FALLBACK_CLIP ? "fallback-clip" :
                      (op->type == GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP ? "fallback-rounded-clip" : "fallback"),
                      g_type_name_from_instance ((GTypeInstance *) node), node,
                      ceil (node->bounds.size.width),
                      ceil (node->bounds.size.height));
#ifdef G_ENABLE_DEBUG
  {
    GskProfiler *profiler = gsk_renderer_get_profiler (gsk_vulkan_render_get_renderer (render));
    gsk_profiler_counter_add (profiler,
                              fallback_pixels_quark,
                              ceil (node->bounds.size.width) * ceil (node->bounds.size.height));
  }
#endif

  /* XXX: We could intersect bounds with clip bounds here */
  width = ceil (node->bounds.size.width * graphene_vec2_get_x (&self->scale));
  height = ceil (node->bounds.size.height * graphene_vec2_get_y (&self->scale));

  op->source = gsk_vulkan_image_new_for_upload (uploader, GDK_MEMORY_DEFAULT, width, height);
  gsk_vulkan_image_map_memory (op->source, uploader, GSK_VULKAN_WRITE, &map);
  surface = cairo_image_surface_create_for_data (map.data,
                                                 CAIRO_FORMAT_ARGB32,
                                                 width, height,
                                                 map.stride);

  cairo_surface_set_device_scale (surface,
                                  width / node->bounds.size.width,
                                  height / node->bounds.size.height);
  cr = cairo_create (surface);
  cairo_translate (cr, -node->bounds.origin.x, -node->bounds.origin.y);

  if (op->type == GSK_VULKAN_OP_FALLBACK_CLIP)
    {
      cairo_rectangle (cr,
                       op->clip.bounds.origin.x - op->offset.x,
                       op->clip.bounds.origin.y - op->offset.y,
                       op->clip.bounds.size.width,
                       op->clip.bounds.size.height);
      cairo_clip (cr);
    }
  else if (op->type == GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP)
    {
      cairo_translate (cr, - op->offset.x, - op->offset.y);
      gsk_rounded_rect_path (&op->clip, cr);
      cairo_translate (cr, op->offset.x, op->offset.y);
      cairo_clip (cr);
    }
  else
    {
      g_assert (op->type == GSK_VULKAN_OP_FALLBACK);
    }

  gsk_render_node_draw (node, cr);

#ifdef G_ENABLE_DEBUG
  if (GSK_RENDERER_DEBUG_CHECK (gsk_vulkan_render_get_renderer (render), FALLBACK))
    {
      cairo_rectangle (cr,
                       op->clip.bounds.origin.x - op->offset.x,
                       op->clip.bounds.origin.y - op->offset.y,
                       op->clip.bounds.size.width,
                       op->clip.bounds.size.height);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 0.25);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 0.25);
      cairo_fill_preserve (cr);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 1);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 1);
      cairo_stroke (cr);
    }
#endif

  cairo_destroy (cr);

  cairo_surface_finish (surface);
  cairo_surface_destroy (surface);

  gsk_vulkan_image_unmap_memory (op->source, uploader, &map);
  gsk_vulkan_render_add_cleanup_image (render, op->source);

  op->source_rect = GRAPHENE_RECT_INIT(0, 0, 1, 1);
}

static void
get_tex_rect (graphene_rect_t       *tex_coords,
              const graphene_rect_t *rect,
              const graphene_rect_t *tex)
{
  graphene_rect_init (tex_coords,
                      (rect->origin.x - tex->origin.x) / tex->size.width,
                      (rect->origin.y - tex->origin.y) / tex->size.height,
                      rect->size.width / tex->size.width,
                      rect->size.height / tex->size.height);
}

void
gsk_vulkan_render_pass_upload (GskVulkanRenderPass  *self,
                               GskVulkanRender      *render,
                               GskVulkanUploader    *uploader)
{
  GskVulkanOp *op;
  guint i;
  const graphene_rect_t *clip = NULL;
  const graphene_vec2_t *scale = NULL;

  for (i = 0; i < self->render_ops->len; i++)
    {
      op = &g_array_index (self->render_ops, GskVulkanOp, i);

      switch (op->type)
        {
        case GSK_VULKAN_OP_FALLBACK:
        case GSK_VULKAN_OP_FALLBACK_CLIP:
        case GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP:
          gsk_vulkan_render_pass_upload_fallback (self, &op->render, render, uploader);
          break;

        case GSK_VULKAN_OP_TEXT:
        case GSK_VULKAN_OP_COLOR_TEXT:
          {
            op->text.source = gsk_vulkan_renderer_ref_glyph_image (GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                                   uploader,
                                                                   op->text.texture_index);
            gsk_vulkan_render_add_cleanup_image (render, op->text.source);
          }
          break;

        case GSK_VULKAN_OP_TEXTURE:
          {
            op->render.source = gsk_vulkan_renderer_ref_texture_image (GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                                       gsk_texture_node_get_texture (op->render.node),
                                                                       uploader);
            op->render.source_rect = GRAPHENE_RECT_INIT(0, 0, 1, 1);
            gsk_vulkan_render_add_cleanup_image (render, op->render.source);
          }
          break;

        case GSK_VULKAN_OP_TEXTURE_SCALE:
          {
            op->render.source = gsk_vulkan_renderer_ref_texture_image (GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                                       gsk_texture_scale_node_get_texture (op->render.node),
                                                                       uploader);
            op->render.source_rect = GRAPHENE_RECT_INIT(0, 0, 1, 1);
            gsk_vulkan_render_add_cleanup_image (render, op->render.source);
          }
          break;

        case GSK_VULKAN_OP_OPACITY:
          {
            GskRenderNode *child = gsk_opacity_node_get_child (op->render.node);
            graphene_rect_t tex_bounds;

            op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                            render,
                                                                            uploader,
                                                                            child,
                                                                            scale,
                                                                            clip,
                                                                            &op->render.offset,
                                                                            &tex_bounds);
            get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);
          }
          break;

        case GSK_VULKAN_OP_REPEAT:
          {
            GskRenderNode *child = gsk_repeat_node_get_child (op->render.node);
            const graphene_rect_t *child_bounds = gsk_repeat_node_get_child_bounds (op->render.node);
            graphene_rect_t tex_bounds;

            if (!graphene_rect_equal (child_bounds, &child->bounds))
              {
                VkSemaphore semaphore;

                /* We need to create a texture in the right size so that we can repeat it
                 * properly, so even for texture nodes this step is necessary.
                 * We also can't use the clip because of that. */
                vkCreateSemaphore (gdk_vulkan_context_get_device (self->vulkan),
                                   &(VkSemaphoreCreateInfo) {
                                     VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                     NULL,
                                     0
                                   },
                                   NULL,
                                   &semaphore);

                g_array_append_val (self->wait_semaphores, semaphore);

                op->render.source = gsk_vulkan_render_pass_render_offscreen (self->vulkan,
                                                                             render,
                                                                             uploader,
                                                                             semaphore,
                                                                             child,
                                                                             scale,
                                                                             child_bounds);
                get_tex_rect (&op->render.source_rect, &op->render.node->bounds, child_bounds);
              }
            else
              {
                op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                                render,
                                                                                uploader,
                                                                                child,
                                                                                scale,
                                                                                &child->bounds,
                                                                                &GRAPHENE_POINT_INIT (0, 0),
                                                                                &tex_bounds);
                get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);
              }
          }
          break;

        case GSK_VULKAN_OP_BLUR:
          {
            GskRenderNode *child = gsk_blur_node_get_child (op->render.node);
            graphene_rect_t tex_bounds;

            op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                            render,
                                                                            uploader,
                                                                            child,
                                                                            scale,
                                                                            clip,
                                                                            &op->render.offset,
                                                                            &tex_bounds);
            get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);
          }
          break;

        case GSK_VULKAN_OP_COLOR_MATRIX:
          {
            GskRenderNode *child = gsk_color_matrix_node_get_child (op->render.node);
            graphene_rect_t tex_bounds;

            op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                            render,
                                                                            uploader,
                                                                            child,
                                                                            scale,
                                                                            clip,
                                                                            &op->render.offset,
                                                                            &tex_bounds);
            get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);
          }
          break;

        case GSK_VULKAN_OP_CROSS_FADE:
          {
            GskRenderNode *start = gsk_cross_fade_node_get_start_child (op->render.node);
            GskRenderNode *end = gsk_cross_fade_node_get_end_child (op->render.node);
            graphene_rect_t tex_bounds;

            op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                            render,
                                                                            uploader,
                                                                            start,
                                                                            scale,
                                                                            clip,
                                                                            &op->render.offset,
                                                                            &tex_bounds);
            get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);

            op->render.source2 = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                             render,
                                                                             uploader,
                                                                             end,
                                                                             scale,
                                                                             clip,
                                                                             &op->render.offset,
                                                                             &tex_bounds);
            get_tex_rect (&op->render.source2_rect, &op->render.node->bounds, &tex_bounds);
            if (!op->render.source)
              {
                op->render.source = op->render.source2;
                op->render.source_rect = *graphene_rect_zero();
              }
            if (!op->render.source2)
              {
                op->render.source2 = op->render.source;
                op->render.source2_rect = *graphene_rect_zero();
              }
          }
          break;

        case GSK_VULKAN_OP_BLEND_MODE:
          {
            GskRenderNode *top = gsk_blend_node_get_top_child (op->render.node);
            GskRenderNode *bottom = gsk_blend_node_get_bottom_child (op->render.node);
            graphene_rect_t tex_bounds;

            op->render.source = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                            render,
                                                                            uploader,
                                                                            top,
                                                                            scale,
                                                                            clip,
                                                                            &op->render.offset,
                                                                            &tex_bounds);
            get_tex_rect (&op->render.source_rect, &op->render.node->bounds, &tex_bounds);

            op->render.source2 = gsk_vulkan_render_pass_get_node_as_texture (self,
                                                                             render,
                                                                             uploader,
                                                                             bottom,
                                                                             scale,
                                                                             clip,
                                                                             &op->render.offset,
                                                                             &tex_bounds);
            get_tex_rect (&op->render.source2_rect, &op->render.node->bounds, &tex_bounds);
            if (!op->render.source)
              {
                op->render.source = op->render.source2;
                op->render.source_rect = *graphene_rect_zero();
              }
            if (!op->render.source2)
              {
                op->render.source2 = op->render.source;
                op->render.source2_rect = *graphene_rect_zero();
              }
          }
          break;

        case GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS:
          clip = &op->constants.clip.bounds;
          scale = &op->constants.scale;
          break;

        default:
          g_assert_not_reached ();
        case GSK_VULKAN_OP_COLOR:
        case GSK_VULKAN_OP_LINEAR_GRADIENT:
        case GSK_VULKAN_OP_BORDER:
        case GSK_VULKAN_OP_INSET_SHADOW:
        case GSK_VULKAN_OP_OUTSET_SHADOW:
        case GSK_VULKAN_OP_SCISSOR:
          break;
        }
    }
}

static inline gsize
round_up (gsize number, gsize divisor)
{
  return (number + divisor - 1) / divisor * divisor;
}

static gsize
gsk_vulkan_render_pass_count_vertex_data (GskVulkanRenderPass *self)
{
  GskVulkanOp *op;
  gsize n_bytes, vertex_stride;
  guint i;

  n_bytes = 0;
  for (i = 0; i < self->render_ops->len; i++)
    {
      op = &g_array_index (self->render_ops, GskVulkanOp, i);

      switch (op->type)
        {
        case GSK_VULKAN_OP_FALLBACK:
        case GSK_VULKAN_OP_FALLBACK_CLIP:
        case GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP:
        case GSK_VULKAN_OP_TEXTURE:
        case GSK_VULKAN_OP_TEXTURE_SCALE:
        case GSK_VULKAN_OP_REPEAT:
        case GSK_VULKAN_OP_COLOR:
        case GSK_VULKAN_OP_LINEAR_GRADIENT:
        case GSK_VULKAN_OP_OPACITY:
        case GSK_VULKAN_OP_COLOR_MATRIX:
        case GSK_VULKAN_OP_BLUR:
        case GSK_VULKAN_OP_BORDER:
        case GSK_VULKAN_OP_INSET_SHADOW:
        case GSK_VULKAN_OP_OUTSET_SHADOW:
        case GSK_VULKAN_OP_CROSS_FADE:
        case GSK_VULKAN_OP_BLEND_MODE:
          vertex_stride = gsk_vulkan_pipeline_get_vertex_stride (op->render.pipeline);
          n_bytes = round_up (n_bytes, vertex_stride);
          op->render.vertex_offset = n_bytes;
          n_bytes += vertex_stride;
          break;

        case GSK_VULKAN_OP_TEXT:
        case GSK_VULKAN_OP_COLOR_TEXT:
          vertex_stride = gsk_vulkan_pipeline_get_vertex_stride (op->render.pipeline);
          n_bytes = round_up (n_bytes, vertex_stride);
          op->text.vertex_offset = n_bytes;
          n_bytes += vertex_stride * op->text.num_glyphs;
          break;

        default:
          g_assert_not_reached ();

        case GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS:
        case GSK_VULKAN_OP_SCISSOR:
          continue;
        }
    }

  return n_bytes;
}

static void
gsk_vulkan_render_pass_collect_vertex_data (GskVulkanRenderPass *self,
                                            GskVulkanRender     *render,
                                            guchar              *data)
{
  GskVulkanOp *op;
  guint i;

  for (i = 0; i < self->render_ops->len; i++)
    {
      op = &g_array_index (self->render_ops, GskVulkanOp, i);

      switch (op->type)
        {
        case GSK_VULKAN_OP_FALLBACK:
        case GSK_VULKAN_OP_FALLBACK_CLIP:
        case GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP:
        case GSK_VULKAN_OP_TEXTURE:
        case GSK_VULKAN_OP_TEXTURE_SCALE:
          gsk_vulkan_texture_pipeline_collect_vertex_data (GSK_VULKAN_TEXTURE_PIPELINE (op->render.pipeline),
                                                           data + op->render.vertex_offset,
                                                           op->render.image_descriptor,
                                                           &op->render.offset,
                                                           &op->render.node->bounds,
                                                           &op->render.source_rect);
          break;

        case GSK_VULKAN_OP_REPEAT:
          gsk_vulkan_texture_pipeline_collect_vertex_data (GSK_VULKAN_TEXTURE_PIPELINE (op->render.pipeline),
                                                           data + op->render.vertex_offset,
                                                           op->render.image_descriptor,
                                                           &op->render.offset,
                                                           &op->render.node->bounds,
                                                           &op->render.source_rect);
          break;

        case GSK_VULKAN_OP_TEXT:
          gsk_vulkan_text_pipeline_collect_vertex_data (GSK_VULKAN_TEXT_PIPELINE (op->text.pipeline),
                                                        data + op->text.vertex_offset,
                                                        GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                        &op->text.node->bounds,
                                                        op->text.image_descriptor,
                                                        (PangoFont *)gsk_text_node_get_font (op->text.node),
                                                        gsk_text_node_get_num_glyphs (op->text.node),
                                                        gsk_text_node_get_glyphs (op->text.node, NULL),
                                                        gsk_text_node_get_color (op->text.node),
                                                        &GRAPHENE_POINT_INIT (
                                                          gsk_text_node_get_offset (op->text.node)->x + op->render.offset.x,
                                                          gsk_text_node_get_offset (op->text.node)->y + op->render.offset.y
                                                        ),
                                                        op->text.start_glyph,
                                                        op->text.num_glyphs,
                                                        op->text.scale);
          break;

        case GSK_VULKAN_OP_COLOR_TEXT:
          gsk_vulkan_color_text_pipeline_collect_vertex_data (GSK_VULKAN_COLOR_TEXT_PIPELINE (op->text.pipeline),
                                                              data + op->text.vertex_offset,
                                                              GSK_VULKAN_RENDERER (gsk_vulkan_render_get_renderer (render)),
                                                              &op->text.node->bounds,
                                                              op->text.image_descriptor,
                                                              (PangoFont *)gsk_text_node_get_font (op->text.node),
                                                              gsk_text_node_get_num_glyphs (op->text.node),
                                                              gsk_text_node_get_glyphs (op->text.node, NULL),
                                                              &GRAPHENE_POINT_INIT (
                                                                gsk_text_node_get_offset (op->text.node)->x + op->render.offset.x,
                                                                gsk_text_node_get_offset (op->text.node)->y + op->render.offset.y
                                                              ),
                                                              op->text.start_glyph,
                                                              op->text.num_glyphs,
                                                              op->text.scale);
          break;

        case GSK_VULKAN_OP_COLOR:
          gsk_vulkan_color_pipeline_collect_vertex_data (GSK_VULKAN_COLOR_PIPELINE (op->render.pipeline),
                                                         data + op->render.vertex_offset,
                                                         &op->render.offset,
                                                         &op->render.node->bounds,
                                                         gsk_color_node_get_color (op->render.node));
          break;

        case GSK_VULKAN_OP_LINEAR_GRADIENT:
          gsk_vulkan_linear_gradient_pipeline_collect_vertex_data (GSK_VULKAN_LINEAR_GRADIENT_PIPELINE (op->render.pipeline),
                                                                   data + op->render.vertex_offset,
                                                                   &op->render.offset,
                                                                   &op->render.node->bounds,
                                                                   gsk_linear_gradient_node_get_start (op->render.node),
                                                                   gsk_linear_gradient_node_get_end (op->render.node),
                                                                   gsk_render_node_get_node_type (op->render.node) == GSK_REPEATING_LINEAR_GRADIENT_NODE,
                                                                   op->render.buffer_offset,
                                                                   gsk_linear_gradient_node_get_n_color_stops (op->render.node));
          break;

        case GSK_VULKAN_OP_OPACITY:
          {
            graphene_matrix_t color_matrix;
            graphene_vec4_t color_offset;

            graphene_matrix_init_from_float (&color_matrix,
                                             (float[16]) {
                                                 1.0, 0.0, 0.0, 0.0,
                                                 0.0, 1.0, 0.0, 0.0,
                                                 0.0, 0.0, 1.0, 0.0,
                                                 0.0, 0.0, 0.0, gsk_opacity_node_get_opacity (op->render.node)
                                             });
            graphene_vec4_init (&color_offset, 0.0, 0.0, 0.0, 0.0);

            gsk_vulkan_effect_pipeline_collect_vertex_data (GSK_VULKAN_EFFECT_PIPELINE (op->render.pipeline),
                                                            data + op->render.vertex_offset,
                                                            op->render.image_descriptor,
                                                            &op->render.offset,
                                                            &op->render.node->bounds,
                                                            &op->render.source_rect,
                                                            &color_matrix,
                                                            &color_offset);
          }
          break;

        case GSK_VULKAN_OP_BLUR:
          gsk_vulkan_blur_pipeline_collect_vertex_data (GSK_VULKAN_BLUR_PIPELINE (op->render.pipeline),
                                                        data + op->render.vertex_offset,
                                                        op->render.image_descriptor,
                                                        &op->render.offset,
                                                        &op->render.node->bounds,
                                                        &op->render.source_rect,
                                                        gsk_blur_node_get_radius (op->render.node));
          break;

        case GSK_VULKAN_OP_COLOR_MATRIX:
          gsk_vulkan_effect_pipeline_collect_vertex_data (GSK_VULKAN_EFFECT_PIPELINE (op->render.pipeline),
                                                          data + op->render.vertex_offset,
                                                          op->render.image_descriptor,
                                                          &op->render.offset,
                                                          &op->render.node->bounds,
                                                          &op->render.source_rect,
                                                          gsk_color_matrix_node_get_color_matrix (op->render.node),
                                                          gsk_color_matrix_node_get_color_offset (op->render.node));
          break;

        case GSK_VULKAN_OP_BORDER:
          gsk_vulkan_border_pipeline_collect_vertex_data (GSK_VULKAN_BORDER_PIPELINE (op->render.pipeline),
                                                          data + op->render.vertex_offset,
                                                          &op->render.offset,
                                                          gsk_border_node_get_outline (op->render.node),
                                                          gsk_border_node_get_widths (op->render.node),
                                                          gsk_border_node_get_colors (op->render.node));
          break;

        case GSK_VULKAN_OP_INSET_SHADOW:
          gsk_vulkan_box_shadow_pipeline_collect_vertex_data (GSK_VULKAN_BOX_SHADOW_PIPELINE (op->render.pipeline),
                                                              data + op->render.vertex_offset,
                                                              &op->render.offset,
                                                              gsk_inset_shadow_node_get_outline (op->render.node),
                                                              gsk_inset_shadow_node_get_color (op->render.node),
                                                              gsk_inset_shadow_node_get_dx (op->render.node),
                                                              gsk_inset_shadow_node_get_dy (op->render.node),
                                                              gsk_inset_shadow_node_get_spread (op->render.node),
                                                              gsk_inset_shadow_node_get_blur_radius (op->render.node));
          break;

        case GSK_VULKAN_OP_OUTSET_SHADOW:
          gsk_vulkan_box_shadow_pipeline_collect_vertex_data (GSK_VULKAN_BOX_SHADOW_PIPELINE (op->render.pipeline),
                                                              data + op->render.vertex_offset,
                                                              &op->render.offset,
                                                              gsk_outset_shadow_node_get_outline (op->render.node),
                                                              gsk_outset_shadow_node_get_color (op->render.node),
                                                              gsk_outset_shadow_node_get_dx (op->render.node),
                                                              gsk_outset_shadow_node_get_dy (op->render.node),
                                                              gsk_outset_shadow_node_get_spread (op->render.node),
                                                              gsk_outset_shadow_node_get_blur_radius (op->render.node));
          break;

        case GSK_VULKAN_OP_CROSS_FADE:
          gsk_vulkan_cross_fade_pipeline_collect_vertex_data (GSK_VULKAN_CROSS_FADE_PIPELINE (op->render.pipeline),
                                                              data + op->render.vertex_offset,
                                                              op->render.image_descriptor,
                                                              op->render.image_descriptor2,
                                                              &op->render.offset,
                                                              &op->render.node->bounds,
                                                              &gsk_cross_fade_node_get_start_child (op->render.node)->bounds,
                                                              &gsk_cross_fade_node_get_end_child (op->render.node)->bounds,
                                                              &op->render.source_rect,
                                                              &op->render.source2_rect,
                                                              gsk_cross_fade_node_get_progress (op->render.node));
          break;

        case GSK_VULKAN_OP_BLEND_MODE:
          gsk_vulkan_blend_mode_pipeline_collect_vertex_data (GSK_VULKAN_BLEND_MODE_PIPELINE (op->render.pipeline),
                                                              data + op->render.vertex_offset,
                                                              op->render.image_descriptor,
                                                              op->render.image_descriptor2,
                                                              &op->render.offset,
                                                              &op->render.node->bounds,
                                                              &gsk_blend_node_get_top_child (op->render.node)->bounds,
                                                              &gsk_blend_node_get_bottom_child (op->render.node)->bounds,
                                                              &op->render.source_rect,
                                                              &op->render.source2_rect,
                                                              gsk_blend_node_get_blend_mode (op->render.node));
          break;

        default:
          g_assert_not_reached ();
        case GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS:
        case GSK_VULKAN_OP_SCISSOR:
          continue;
        }
    }
}

static GskVulkanBuffer *
gsk_vulkan_render_pass_get_vertex_data (GskVulkanRenderPass *self,
                                        GskVulkanRender     *render)
{
  if (self->vertex_data == NULL)
    {
      gsize n_bytes;
      guchar *data;

      n_bytes = gsk_vulkan_render_pass_count_vertex_data (self);
      if (n_bytes == 0)
        return NULL;

      self->vertex_data = gsk_vulkan_buffer_new (self->vulkan, n_bytes);
      data = gsk_vulkan_buffer_map (self->vertex_data);
      gsk_vulkan_render_pass_collect_vertex_data (self, render, data);
      gsk_vulkan_buffer_unmap (self->vertex_data);
    }

  return self->vertex_data;
}

gsize
gsk_vulkan_render_pass_get_wait_semaphores (GskVulkanRenderPass  *self,
                                            VkSemaphore         **semaphores)
{
  *semaphores = (VkSemaphore *)self->wait_semaphores->data;
  return self->wait_semaphores->len;
}

gsize
gsk_vulkan_render_pass_get_signal_semaphores (GskVulkanRenderPass  *self,
                                              VkSemaphore         **semaphores)
{
  *semaphores = (VkSemaphore *)&self->signal_semaphore;
  return self->signal_semaphore != VK_NULL_HANDLE ? 1 : 0;
}

void
gsk_vulkan_render_pass_reserve_descriptor_sets (GskVulkanRenderPass *self,
                                                GskVulkanRender     *render)
{
  GskVulkanOp *op;
  guint i;

  for (i = 0; i < self->render_ops->len; i++)
    {
      op = &g_array_index (self->render_ops, GskVulkanOp, i);

      switch (op->type)
        {
        case GSK_VULKAN_OP_FALLBACK:
        case GSK_VULKAN_OP_FALLBACK_CLIP:
        case GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP:
        case GSK_VULKAN_OP_TEXTURE:
        case GSK_VULKAN_OP_OPACITY:
        case GSK_VULKAN_OP_BLUR:
        case GSK_VULKAN_OP_COLOR_MATRIX:
          if (op->render.source)
            {
              op->render.image_descriptor[0] = gsk_vulkan_render_get_image_descriptor (render, op->render.source);
              op->render.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_DEFAULT);
            }
          break;

        case GSK_VULKAN_OP_TEXTURE_SCALE:
          if (op->render.source)
            {
              op->render.image_descriptor[0] = gsk_vulkan_render_get_image_descriptor (render, op->render.source);
              switch (gsk_texture_scale_node_get_filter (op->render.node))
                {
                default:
                  g_assert_not_reached ();
                case GSK_SCALING_FILTER_LINEAR:
                case GSK_SCALING_FILTER_TRILINEAR:
                  op->render.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_DEFAULT);
                  break;
                case GSK_SCALING_FILTER_NEAREST:
                  op->render.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_NEAREST);
                  break;
                }
            }
          break;

        case GSK_VULKAN_OP_REPEAT:
          if (op->render.source)
            {
              op->render.image_descriptor[0] = gsk_vulkan_render_get_image_descriptor (render, op->render.source);
              op->render.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_REPEAT);
            }
          break;

        case GSK_VULKAN_OP_TEXT:
        case GSK_VULKAN_OP_COLOR_TEXT:
          op->text.image_descriptor[0] = gsk_vulkan_render_get_image_descriptor (render, op->text.source);
          op->text.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_DEFAULT);
          break;

        case GSK_VULKAN_OP_CROSS_FADE:
        case GSK_VULKAN_OP_BLEND_MODE:
          if (op->render.source && op->render.source2)
            {
              op->render.image_descriptor[0] = gsk_vulkan_render_get_image_descriptor (render, op->render.source);
              op->render.image_descriptor[1] = gsk_vulkan_render_get_sampler_descriptor (render, GSK_VULKAN_SAMPLER_DEFAULT);
              op->render.image_descriptor2[0] = gsk_vulkan_render_get_image_descriptor (render, op->render.source2);
              op->render.image_descriptor2[1] = op->render.image_descriptor2[1];
            }
          break;

        case GSK_VULKAN_OP_LINEAR_GRADIENT:
          {
            gsize n_stops = gsk_linear_gradient_node_get_n_color_stops (op->render.node);
            guchar *mem;

            mem = gsk_vulkan_render_get_buffer_memory (render,
                                                       n_stops * sizeof (GskColorStop),
                                                       G_ALIGNOF (GskColorStop),
                                                       &op->render.buffer_offset);
            memcpy (mem,
                    gsk_linear_gradient_node_get_color_stops (op->render.node, NULL),
                    n_stops * sizeof (GskColorStop));
          }
          break;

        default:
          g_assert_not_reached ();

        case GSK_VULKAN_OP_COLOR:
        case GSK_VULKAN_OP_BORDER:
        case GSK_VULKAN_OP_INSET_SHADOW:
        case GSK_VULKAN_OP_OUTSET_SHADOW:
        case GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS:
        case GSK_VULKAN_OP_SCISSOR:
          break;
        }
    }
}

static void
gsk_vulkan_render_pass_draw_rect (GskVulkanRenderPass     *self,
                                  GskVulkanRender         *render,
                                  VkPipelineLayout         pipeline_layout,
                                  VkCommandBuffer          command_buffer)
{
  GskVulkanPipeline *current_pipeline = NULL;
  GskVulkanOp *op;
  guint i, step;
  GskVulkanBuffer *vertex_buffer;

  vertex_buffer = gsk_vulkan_render_pass_get_vertex_data (self, render);

  if (vertex_buffer)
    vkCmdBindVertexBuffers (command_buffer,
                            0,
                            1,
                            (VkBuffer[1]) {
                                gsk_vulkan_buffer_get_buffer (vertex_buffer)
                            },
                            (VkDeviceSize[1]) { 0 });

  for (i = 0; i < self->render_ops->len; i += step)
    {
      op = &g_array_index (self->render_ops, GskVulkanOp, i);
      step = 1;

      switch (op->type)
        {
        case GSK_VULKAN_OP_FALLBACK:
        case GSK_VULKAN_OP_FALLBACK_CLIP:
        case GSK_VULKAN_OP_FALLBACK_ROUNDED_CLIP:
        case GSK_VULKAN_OP_TEXTURE:
        case GSK_VULKAN_OP_TEXTURE_SCALE:
        case GSK_VULKAN_OP_REPEAT:
          if (!op->render.source)
            continue;
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_texture_pipeline_draw (GSK_VULKAN_TEXTURE_PIPELINE (current_pipeline),
                                            command_buffer,
                                            op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                            1);
          break;

        case GSK_VULKAN_OP_TEXT:
          if (current_pipeline != op->text.pipeline)
            {
              current_pipeline = op->text.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_text_pipeline_draw (GSK_VULKAN_TEXT_PIPELINE (current_pipeline),
                                         command_buffer,
                                         op->text.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                         op->text.num_glyphs);
          break;

        case GSK_VULKAN_OP_COLOR_TEXT:
          if (current_pipeline != op->text.pipeline)
            {
              current_pipeline = op->text.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_color_text_pipeline_draw (GSK_VULKAN_COLOR_TEXT_PIPELINE (current_pipeline),
                                               command_buffer,
                                               op->text.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                               op->text.num_glyphs);
          break;

        case GSK_VULKAN_OP_OPACITY:
        case GSK_VULKAN_OP_COLOR_MATRIX:
          if (!op->render.source)
            continue;
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_effect_pipeline_draw (GSK_VULKAN_EFFECT_PIPELINE (current_pipeline),
                                           command_buffer,
                                           op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                           1);
          break;

        case GSK_VULKAN_OP_BLUR:
          if (!op->render.source)
            continue;
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_blur_pipeline_draw (GSK_VULKAN_BLUR_PIPELINE (current_pipeline),
                                         command_buffer,
                                         op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                         1);
          break;

        case GSK_VULKAN_OP_COLOR:
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          for (step = 1; step + i < self->render_ops->len; step++)
            {
              GskVulkanOp *cmp = &g_array_index (self->render_ops, GskVulkanOp, i + step);
              if (cmp->type != GSK_VULKAN_OP_COLOR || 
                  cmp->render.pipeline != current_pipeline)
                break;
            }
          gsk_vulkan_color_pipeline_draw (GSK_VULKAN_COLOR_PIPELINE (current_pipeline),
                                          command_buffer,
                                          op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                          step);
          break;

        case GSK_VULKAN_OP_LINEAR_GRADIENT:
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }
          gsk_vulkan_linear_gradient_pipeline_draw (GSK_VULKAN_LINEAR_GRADIENT_PIPELINE (current_pipeline),
                                                    command_buffer,
                                                    op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                                    1);
          break;

        case GSK_VULKAN_OP_BORDER:
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }
          gsk_vulkan_border_pipeline_draw (GSK_VULKAN_BORDER_PIPELINE (current_pipeline),
                                           command_buffer,
                                           op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                           1);
          break;

        case GSK_VULKAN_OP_INSET_SHADOW:
        case GSK_VULKAN_OP_OUTSET_SHADOW:
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }
          gsk_vulkan_box_shadow_pipeline_draw (GSK_VULKAN_BOX_SHADOW_PIPELINE (current_pipeline),
                                               command_buffer,
                                               op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                               1);
          break;

        case GSK_VULKAN_OP_PUSH_VERTEX_CONSTANTS:
          gsk_vulkan_push_constants_push (command_buffer,
                                          pipeline_layout,
                                          &op->constants.scale,
                                          &op->constants.mvp,
                                          &op->constants.clip);
          break;

        case GSK_VULKAN_OP_SCISSOR:
          vkCmdSetScissor (command_buffer,
                           0,
                           1,
                           &(VkRect2D) {
                             { op->scissor.rect.x, op->scissor.rect.y },
                             { op->scissor.rect.width, op->scissor.rect.height },
                           });
          break;

        case GSK_VULKAN_OP_CROSS_FADE:
          if (!op->render.source || !op->render.source2)
            continue;
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_cross_fade_pipeline_draw (GSK_VULKAN_CROSS_FADE_PIPELINE (current_pipeline),
                                               command_buffer,
                                               op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                               1);
          break;

        case GSK_VULKAN_OP_BLEND_MODE:
          if (!op->render.source || !op->render.source2)
            continue;
          if (current_pipeline != op->render.pipeline)
            {
              current_pipeline = op->render.pipeline;
              vkCmdBindPipeline (command_buffer,
                                 VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 gsk_vulkan_pipeline_get_pipeline (current_pipeline));
            }

          gsk_vulkan_blend_mode_pipeline_draw (GSK_VULKAN_BLEND_MODE_PIPELINE (current_pipeline),
                                               command_buffer,
                                               op->render.vertex_offset / gsk_vulkan_pipeline_get_vertex_stride (current_pipeline),
                                               1);
          break;

        default:
          g_assert_not_reached ();
          break;
        }
    }
}

void
gsk_vulkan_render_pass_draw (GskVulkanRenderPass *self,
                             GskVulkanRender     *render,
                             VkPipelineLayout     pipeline_layout,
                             VkCommandBuffer      command_buffer)
{
  cairo_rectangle_int_t rect;

  vkCmdSetViewport (command_buffer,
                    0,
                    1,
                    &(VkViewport) {
                        .x = 0,
                        .y = 0,
                        .width = self->viewport.size.width,
                        .height = self->viewport.size.height,
                        .minDepth = 0,
                        .maxDepth = 1
                    });

  cairo_region_get_extents (self->clip, &rect);

  vkCmdBeginRenderPass (command_buffer,
                        &(VkRenderPassBeginInfo) {
                            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                            .renderPass = self->render_pass,
                            .framebuffer = self->framebuffer,
                            .renderArea = { 
                                { rect.x, rect.y },
                                { rect.width, rect.height }
                            },
                            .clearValueCount = 1,
                            .pClearValues = (VkClearValue [1]) {
                                { .color = { .float32 = { 0.f, 0.f, 0.f, 0.f } } }
                            }
                        },
                        VK_SUBPASS_CONTENTS_INLINE);

  gsk_vulkan_render_bind_descriptor_sets (render, command_buffer);

  gsk_vulkan_render_pass_draw_rect (self, render, pipeline_layout, command_buffer);

  vkCmdEndRenderPass (command_buffer);
}
