#include "config.h"

#include "gskvulkantextpipelineprivate.h"

#include "vulkan/resources/mask.vert.h"

struct _GskVulkanTextPipeline
{
  GObject parent_instance;
};

G_DEFINE_TYPE (GskVulkanTextPipeline, gsk_vulkan_text_pipeline, GSK_TYPE_VULKAN_PIPELINE)

static const VkPipelineVertexInputStateCreateInfo *
gsk_vulkan_text_pipeline_get_input_state_create_info (GskVulkanPipeline *self)
{
  return &gsk_vulkan_mask_info;
}

static void
gsk_vulkan_text_pipeline_finalize (GObject *gobject)
{
  //GskVulkanTextPipeline *self = GSK_VULKAN_TEXT_PIPELINE (gobject);

  G_OBJECT_CLASS (gsk_vulkan_text_pipeline_parent_class)->finalize (gobject);
}

static void
gsk_vulkan_text_pipeline_class_init (GskVulkanTextPipelineClass *klass)
{
  GskVulkanPipelineClass *pipeline_class = GSK_VULKAN_PIPELINE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = gsk_vulkan_text_pipeline_finalize;

  pipeline_class->get_input_state_create_info = gsk_vulkan_text_pipeline_get_input_state_create_info;
}

static void
gsk_vulkan_text_pipeline_init (GskVulkanTextPipeline *self)
{
}

GskVulkanPipeline *
gsk_vulkan_text_pipeline_new (GdkVulkanContext        *context,
                              VkPipelineLayout         layout,
                              const char              *shader_name,
                              VkRenderPass             render_pass)
{
  return gsk_vulkan_pipeline_new (GSK_TYPE_VULKAN_TEXT_PIPELINE, context, layout, shader_name, render_pass);
}

void
gsk_vulkan_text_pipeline_collect_vertex_data (GskVulkanTextPipeline  *pipeline,
                                              guchar                 *data,
                                              GskVulkanRenderer      *renderer,
                                              const graphene_rect_t  *rect,
                                              guint                   tex_id[2],
                                              PangoFont              *font,
                                              guint                   total_glyphs,
                                              const PangoGlyphInfo   *glyphs,
                                              const GdkRGBA          *color,
                                              const graphene_point_t *offset,
                                              guint                   start_glyph,
                                              guint                   num_glyphs,
                                              float                   scale)
{
  GskVulkanMaskInstance *instances = (GskVulkanMaskInstance *) data;
  int i;
  int count = 0;
  int x_position = 0;

  for (i = 0; i < start_glyph; i++)
    x_position += glyphs[i].geometry.width;

  for (; i < total_glyphs && count < num_glyphs; i++)
    {
      const PangoGlyphInfo *gi = &glyphs[i];

      if (gi->glyph != PANGO_GLYPH_EMPTY)
        {
          double cx = (x_position + gi->geometry.x_offset) / PANGO_SCALE;
          double cy = gi->geometry.y_offset / PANGO_SCALE;
          GskVulkanMaskInstance *instance = &instances[count];
          GskVulkanCachedGlyph *glyph;

          glyph = gsk_vulkan_renderer_get_cached_glyph (renderer,
                                                        font,
                                                        gi->glyph,
                                                        x_position + gi->geometry.x_offset,
                                                        gi->geometry.y_offset,
                                                        scale);

          instance->rect[0] = offset->x + cx + glyph->draw_x;
          instance->rect[1] = offset->y + cy + glyph->draw_y;
          instance->rect[2] = glyph->draw_width;
          instance->rect[3] = glyph->draw_height;

          instance->tex_rect[0] = glyph->tx;
          instance->tex_rect[1] = glyph->ty;
          instance->tex_rect[2] = glyph->tw;
          instance->tex_rect[3] = glyph->th;

          instance->color[0] = color->red;
          instance->color[1] = color->green;
          instance->color[2] = color->blue;
          instance->color[3] = color->alpha;

          instance->tex_id[0] = tex_id[0];
          instance->tex_id[1] = tex_id[1];

          count++;
        }
      x_position += gi->geometry.width;
    }
}

gsize
gsk_vulkan_text_pipeline_draw (GskVulkanTextPipeline *pipeline,
                               VkCommandBuffer        command_buffer,
                               gsize                  offset,
                               gsize                  n_commands)
{
  vkCmdDraw (command_buffer,
             6, n_commands,
             0, offset);

  return n_commands;
}
