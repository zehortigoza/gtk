#include "config.h"

#include "gskvulkanpipelineprivate.h"

#include "gskvulkanpushconstantsprivate.h"
#include "gskvulkanshaderprivate.h"

#include "gdk/gdkvulkancontextprivate.h"

#include <graphene.h>

typedef struct _GskVulkanPipelinePrivate GskVulkanPipelinePrivate;

struct _GskVulkanPipelinePrivate
{
  GObject parent_instance;

  GdkVulkanContext *context;

  VkPipeline pipeline;

  GskVulkanShader *vertex_shader;
  GskVulkanShader *fragment_shader;

  gsize vertex_stride;
};

G_DEFINE_TYPE_WITH_PRIVATE (GskVulkanPipeline, gsk_vulkan_pipeline, G_TYPE_OBJECT)

static void
gsk_vulkan_pipeline_finalize (GObject *gobject)
{
  GskVulkanPipelinePrivate *priv = gsk_vulkan_pipeline_get_instance_private (GSK_VULKAN_PIPELINE (gobject));
  VkDevice device;

  device = gdk_vulkan_context_get_device (priv->context);

  vkDestroyPipeline (device,
                     priv->pipeline,
                     NULL);

  g_clear_pointer (&priv->fragment_shader, gsk_vulkan_shader_free);
  g_clear_pointer (&priv->vertex_shader, gsk_vulkan_shader_free);

  G_OBJECT_CLASS (gsk_vulkan_pipeline_parent_class)->finalize (gobject);
}

static void
gsk_vulkan_pipeline_class_init (GskVulkanPipelineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gsk_vulkan_pipeline_finalize;
}

static void
gsk_vulkan_pipeline_init (GskVulkanPipeline *self)
{
}

GskVulkanPipeline *
gsk_vulkan_pipeline_new (GType             pipeline_type,
                         GdkVulkanContext *context,
                         VkPipelineLayout  layout,
                         const char       *shader_name,
                         VkRenderPass      render_pass)
{
  const VkPipelineVertexInputStateCreateInfo *vertex_input_state;
  GskVulkanPipelinePrivate *priv;
  GskVulkanPipeline *self;
  VkDevice device;

  g_return_val_if_fail (g_type_is_a (pipeline_type, GSK_TYPE_VULKAN_PIPELINE), NULL);
  g_return_val_if_fail (layout != VK_NULL_HANDLE, NULL);
  g_return_val_if_fail (shader_name != NULL, NULL);
  g_return_val_if_fail (render_pass != VK_NULL_HANDLE, NULL);

  self = g_object_new (pipeline_type, NULL);

  priv = gsk_vulkan_pipeline_get_instance_private (self);

  device = gdk_vulkan_context_get_device (context);

  priv->context = context;

  priv->vertex_shader = gsk_vulkan_shader_new_from_resource (context, GSK_VULKAN_SHADER_VERTEX, shader_name, NULL);
  priv->fragment_shader = gsk_vulkan_shader_new_from_resource (context, GSK_VULKAN_SHADER_FRAGMENT, shader_name, NULL);

  vertex_input_state = GSK_VULKAN_PIPELINE_GET_CLASS (self)->get_input_state_create_info (self);
  g_assert (vertex_input_state->vertexBindingDescriptionCount == 1);
  priv->vertex_stride = vertex_input_state->pVertexBindingDescriptions[0].stride;

  GSK_VK_CHECK (vkCreateGraphicsPipelines, device,
                                           gdk_vulkan_context_get_pipeline_cache (context),
                                           1,
                                           &(VkGraphicsPipelineCreateInfo) {
                                               .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = (VkPipelineShaderStageCreateInfo[2]) {
                                                   GST_VULKAN_SHADER_STAGE_CREATE_INFO (priv->vertex_shader),
                                                   GST_VULKAN_SHADER_STAGE_CREATE_INFO (priv->fragment_shader)
                                               },
                                               .pVertexInputState = vertex_input_state,
                                               .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                   .primitiveRestartEnable = VK_FALSE,
                                               },
                                               .pTessellationState = NULL,
                                               .pViewportState = &(VkPipelineViewportStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                   .viewportCount = 1,
                                                   .scissorCount = 1
                                               },
                                               .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .depthClampEnable = VK_FALSE,
                                                   .rasterizerDiscardEnable = VK_FALSE,
                                                   .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE,
                                                   .frontFace = VK_FRONT_FACE_CLOCKWISE,
                                                   .lineWidth = 1.0f,
                                               },
                                               .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = 1,
                                               },
                                               .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
                                               },
                                               .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                   .attachmentCount = 1,
                                                   .pAttachments = (VkPipelineColorBlendAttachmentState []) {
                                                       {
                                                           .blendEnable = VK_TRUE,
                                                           .colorBlendOp = VK_BLEND_OP_ADD,
                                                           .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                           .alphaBlendOp = VK_BLEND_OP_ADD,
                                                           .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                           .colorWriteMask = VK_COLOR_COMPONENT_A_BIT
                                                                           | VK_COLOR_COMPONENT_R_BIT
                                                                           | VK_COLOR_COMPONENT_G_BIT
                                                                           | VK_COLOR_COMPONENT_B_BIT
                                                       },
                                                   }
                                               },
                                               .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 2,
                                                   .pDynamicStates = (VkDynamicState[2]) {
                                                       VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR
                                                   },
                                               },
                                               .layout = layout,
                                               .renderPass = render_pass,
                                               .subpass = 0,
                                               .basePipelineHandle = VK_NULL_HANDLE,
                                               .basePipelineIndex = -1,
                                           },
                                           NULL,
                                           &priv->pipeline);

  gdk_vulkan_context_pipeline_cache_updated (context);

  return self;
}

VkPipeline
gsk_vulkan_pipeline_get_pipeline (GskVulkanPipeline *self)
{
  GskVulkanPipelinePrivate *priv = gsk_vulkan_pipeline_get_instance_private (self);

  return priv->pipeline;
}

gsize
gsk_vulkan_pipeline_get_vertex_stride (GskVulkanPipeline *self)
{
  GskVulkanPipelinePrivate *priv = gsk_vulkan_pipeline_get_instance_private (self);

  return priv->vertex_stride;
}
