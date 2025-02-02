#include "config.h"

#include "gskvulkantexturepipelineprivate.h"

#include "vulkan/resources/texture.vert.h"

struct _GskVulkanTexturePipeline
{
  GObject parent_instance;
};

G_DEFINE_TYPE (GskVulkanTexturePipeline, gsk_vulkan_texture_pipeline, GSK_TYPE_VULKAN_PIPELINE)

static const VkPipelineVertexInputStateCreateInfo *
gsk_vulkan_texture_pipeline_get_input_state_create_info (GskVulkanPipeline *self)
{
  return &gsk_vulkan_texture_info;
}

static void
gsk_vulkan_texture_pipeline_finalize (GObject *gobject)
{
  //GskVulkanTexturePipeline *self = GSK_VULKAN_TEXTURE_PIPELINE (gobject);

  G_OBJECT_CLASS (gsk_vulkan_texture_pipeline_parent_class)->finalize (gobject);
}

static void
gsk_vulkan_texture_pipeline_class_init (GskVulkanTexturePipelineClass *klass)
{
  GskVulkanPipelineClass *pipeline_class = GSK_VULKAN_PIPELINE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = gsk_vulkan_texture_pipeline_finalize;

  pipeline_class->get_input_state_create_info = gsk_vulkan_texture_pipeline_get_input_state_create_info;
}

static void
gsk_vulkan_texture_pipeline_init (GskVulkanTexturePipeline *self)
{
}

GskVulkanPipeline *
gsk_vulkan_texture_pipeline_new (GdkVulkanContext *context,
                                 VkPipelineLayout  layout,
                                 const char       *shader_name,
                                 VkRenderPass      render_pass)
{
  return gsk_vulkan_pipeline_new (GSK_TYPE_VULKAN_TEXTURE_PIPELINE, context, layout, shader_name, render_pass);
}

void
gsk_vulkan_texture_pipeline_collect_vertex_data (GskVulkanTexturePipeline *pipeline,
                                                 guchar                   *data,
                                                 guint32                   tex_id[2],
                                                 const graphene_point_t   *offset,
                                                 const graphene_rect_t    *rect,
                                                 const graphene_rect_t    *tex_rect)
{
  GskVulkanTextureInstance *instance = (GskVulkanTextureInstance *) data;

  instance->rect[0] = rect->origin.x + offset->x;
  instance->rect[1] = rect->origin.y + offset->y;
  instance->rect[2] = rect->size.width;
  instance->rect[3] = rect->size.height;
  instance->tex_rect[0] = tex_rect->origin.x;
  instance->tex_rect[1] = tex_rect->origin.y;
  instance->tex_rect[2] = tex_rect->size.width;
  instance->tex_rect[3] = tex_rect->size.height;
  instance->tex_id[0] = tex_id[0];
  instance->tex_id[1] = tex_id[1];
}

gsize
gsk_vulkan_texture_pipeline_draw (GskVulkanTexturePipeline *pipeline,
                                  VkCommandBuffer           command_buffer,
                                  gsize                     offset,
                                  gsize                     n_commands)
{
  vkCmdDraw (command_buffer,
             6, n_commands,
             0, offset);

  return n_commands;
}
