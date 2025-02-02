#pragma once

#include <graphene.h>

#include "gskvulkanpipelineprivate.h"

G_BEGIN_DECLS

typedef struct _GskVulkanEffectPipelineLayout GskVulkanEffectPipelineLayout;

#define GSK_TYPE_VULKAN_EFFECT_PIPELINE (gsk_vulkan_effect_pipeline_get_type ())

G_DECLARE_FINAL_TYPE (GskVulkanEffectPipeline, gsk_vulkan_effect_pipeline, GSK, VULKAN_EFFECT_PIPELINE, GskVulkanPipeline)

GskVulkanPipeline *     gsk_vulkan_effect_pipeline_new                  (GdkVulkanContext               *context,
                                                                         VkPipelineLayout                layout,
                                                                         const char                     *shader_name,
                                                                         VkRenderPass                    render_pass);

void                    gsk_vulkan_effect_pipeline_collect_vertex_data  (GskVulkanEffectPipeline        *pipeline,
                                                                         guchar                         *data,
                                                                         guint32                         tex_id[2],
                                                                         const graphene_point_t         *offset,
                                                                         const graphene_rect_t          *rect,
                                                                         const graphene_rect_t          *tex_rect,
                                                                         const graphene_matrix_t        *color_matrix,
                                                                         const graphene_vec4_t          *color_offset);
gsize                   gsk_vulkan_effect_pipeline_draw                 (GskVulkanEffectPipeline        *pipeline,
                                                                         VkCommandBuffer                 command_buffer,
                                                                         gsize                           offset,
                                                                         gsize                           n_commands);

G_END_DECLS

