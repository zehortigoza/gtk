#pragma once

#include <gdk/gdk.h>
#include <gsk/gskrendernode.h>

#include "gskvulkanimageprivate.h"
#include "gskvulkanpipelineprivate.h"
#include "gskvulkanrenderpassprivate.h"
#include "gsk/gskprivate.h"

G_BEGIN_DECLS

typedef enum {
  GSK_VULKAN_PIPELINE_TEXTURE,
  GSK_VULKAN_PIPELINE_TEXTURE_CLIP,
  GSK_VULKAN_PIPELINE_TEXTURE_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_COLOR,
  GSK_VULKAN_PIPELINE_COLOR_CLIP,
  GSK_VULKAN_PIPELINE_COLOR_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_LINEAR_GRADIENT,
  GSK_VULKAN_PIPELINE_LINEAR_GRADIENT_CLIP,
  GSK_VULKAN_PIPELINE_LINEAR_GRADIENT_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_COLOR_MATRIX,
  GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP,
  GSK_VULKAN_PIPELINE_COLOR_MATRIX_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_BORDER,
  GSK_VULKAN_PIPELINE_BORDER_CLIP,
  GSK_VULKAN_PIPELINE_BORDER_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_INSET_SHADOW,
  GSK_VULKAN_PIPELINE_INSET_SHADOW_CLIP,
  GSK_VULKAN_PIPELINE_INSET_SHADOW_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_OUTSET_SHADOW,
  GSK_VULKAN_PIPELINE_OUTSET_SHADOW_CLIP,
  GSK_VULKAN_PIPELINE_OUTSET_SHADOW_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_BLUR,
  GSK_VULKAN_PIPELINE_BLUR_CLIP,
  GSK_VULKAN_PIPELINE_BLUR_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_TEXT,
  GSK_VULKAN_PIPELINE_TEXT_CLIP,
  GSK_VULKAN_PIPELINE_TEXT_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_COLOR_TEXT,
  GSK_VULKAN_PIPELINE_COLOR_TEXT_CLIP,
  GSK_VULKAN_PIPELINE_COLOR_TEXT_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_CROSS_FADE,
  GSK_VULKAN_PIPELINE_CROSS_FADE_CLIP,
  GSK_VULKAN_PIPELINE_CROSS_FADE_CLIP_ROUNDED,
  GSK_VULKAN_PIPELINE_BLEND_MODE,
  GSK_VULKAN_PIPELINE_BLEND_MODE_CLIP,
  GSK_VULKAN_PIPELINE_BLEND_MODE_CLIP_ROUNDED,
  /* add more */
  GSK_VULKAN_N_PIPELINES
} GskVulkanPipelineType;

typedef enum {
  GSK_VULKAN_SAMPLER_DEFAULT,
  GSK_VULKAN_SAMPLER_REPEAT,
  GSK_VULKAN_SAMPLER_NEAREST
} GskVulkanRenderSampler;

GskVulkanRender *       gsk_vulkan_render_new                           (GskRenderer            *renderer,
                                                                         GdkVulkanContext       *context);
void                    gsk_vulkan_render_free                          (GskVulkanRender        *self);

gboolean                gsk_vulkan_render_is_busy                       (GskVulkanRender        *self);
void                    gsk_vulkan_render_reset                         (GskVulkanRender        *self,
                                                                         GskVulkanImage         *target,
                                                                         const graphene_rect_t  *rect,
                                                                         const cairo_region_t   *clip);

GskRenderer *           gsk_vulkan_render_get_renderer                  (GskVulkanRender        *self);

void                    gsk_vulkan_render_add_cleanup_image             (GskVulkanRender        *self,
                                                                         GskVulkanImage         *image);

void                    gsk_vulkan_render_add_node                      (GskVulkanRender        *self,
                                                                         GskRenderNode          *node);

void                    gsk_vulkan_render_add_render_pass               (GskVulkanRender        *self,
                                                                         GskVulkanRenderPass    *pass);

void                    gsk_vulkan_render_upload                        (GskVulkanRender        *self);

GskVulkanPipeline *     gsk_vulkan_render_get_pipeline                  (GskVulkanRender        *self,
                                                                         GskVulkanPipelineType   pipeline_type,
                                                                         VkRenderPass            render_pass);
gsize                   gsk_vulkan_render_get_sampler_descriptor        (GskVulkanRender        *self,
                                                                         GskVulkanRenderSampler  render_sampler);
gsize                   gsk_vulkan_render_get_image_descriptor          (GskVulkanRender        *self,
                                                                         GskVulkanImage         *source);
gsize                   gsk_vulkan_render_get_buffer_descriptor         (GskVulkanRender        *self,
                                                                         GskVulkanBuffer        *buffer);
guchar *                gsk_vulkan_render_get_buffer_memory             (GskVulkanRender        *self,
                                                                         gsize                   size,
                                                                         gsize                   alignment,
                                                                         gsize                  *out_offset);
void                    gsk_vulkan_render_bind_descriptor_sets          (GskVulkanRender        *self,
                                                                         VkCommandBuffer         command_buffer);

void                    gsk_vulkan_render_draw                          (GskVulkanRender        *self);

void                    gsk_vulkan_render_submit                        (GskVulkanRender        *self);

GdkTexture *            gsk_vulkan_render_download_target               (GskVulkanRender        *self);
VkFramebuffer           gsk_vulkan_render_get_framebuffer               (GskVulkanRender        *self,
                                                                         GskVulkanImage         *image);
VkFence                 gsk_vulkan_render_get_fence                     (GskVulkanRender        *self);

G_END_DECLS

