#pragma once

#include "cookie.hpp"
#include "hashmap.hpp"
#include "intrusive.hpp"
#include "limits.hpp"
#include "object_pool.hpp"
#include "vulkan.hpp"

namespace Vulkan
{
enum RenderPassOp
{
	RENDER_PASS_OP_CLEAR_COLOR_BIT = 1 << 0,
	RENDER_PASS_OP_LOAD_COLOR_BIT = 1 << 1,
	RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT = 1 << 2,
	RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT = 1 << 3,

	RENDER_PASS_OP_STORE_COLOR_BIT = 1 << 4,
	RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT = 1 << 5,

	RENDER_PASS_OP_COLOR_OPTIMAL_BIT = 1 << 6,
	RENDER_PASS_OP_DEPTH_STENCIL_OPTIMAL_BIT = 1 << 7,

	RENDER_PASS_OP_CLEAR_ALL_BIT = RENDER_PASS_OP_CLEAR_COLOR_BIT | RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT,

	RENDER_PASS_OP_LOAD_ALL_BIT = RENDER_PASS_OP_LOAD_COLOR_BIT | RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT,

	RENDER_PASS_OP_STORE_ALL_BIT = RENDER_PASS_OP_STORE_COLOR_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT,
};
using RenderPassOpFlags = uint32_t;

class ImageView;
struct RenderPassInfo
{
	ImageView *color_attachments[VULKAN_NUM_ATTACHMENTS] = {};
	ImageView *depth_stencil = nullptr;
	unsigned num_color_attachments = 0;
	RenderPassOpFlags op_flags = 0;

	// Render area will be clipped to the actual framebuffer.
	VkRect2D render_area = { { 0, 0 }, { UINT32_MAX, UINT32_MAX } };

	VkClearColorValue clear_color[VULKAN_NUM_ATTACHMENTS] = {};
	VkClearDepthStencilValue clear_depth_stencil = { 1.0f, 0 };
};

class RenderPass : public Cookie
{
public:
	RenderPass(Device *device, const RenderPassInfo &info);
	~RenderPass();

	VkRenderPass get_render_pass() const
	{
		return render_pass;
	}

private:
	Device *device;
	VkRenderPass render_pass = VK_NULL_HANDLE;

	VkFormat color_attachments[VULKAN_NUM_ATTACHMENTS];
	VkFormat depth_stencil;
	unsigned num_color_attachments;
};

class Framebuffer : public Cookie
{
public:
	Framebuffer(Device *device, const RenderPass &rp, const RenderPassInfo &info);
	~Framebuffer();

	VkFramebuffer get_framebuffer() const
	{
		return framebuffer;
	}

	uint32_t get_width() const
	{
		return width;
	}

	uint32_t get_height() const
	{
		return height;
	}

	const RenderPass &get_render_pass() const
	{
		return render_pass;
	}

private:
	Device *device;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	const RenderPass &render_pass;
	RenderPassInfo info;
	uint32_t width = 0;
	uint32_t height = 0;
};

static const unsigned VULKAN_FRAMEBUFFER_RING_SIZE = 4;
class FramebufferAllocator
{
public:
	FramebufferAllocator(Device *device);
	~FramebufferAllocator();
	Framebuffer &request_framebuffer(const RenderPassInfo &info);

	void begin_frame();

private:
	struct FramebufferNode : public IntrusiveListEnabled<FramebufferNode>, public Framebuffer
	{
		FramebufferNode(Device *device, const RenderPass &rp, const RenderPassInfo &info)
		    : Framebuffer(device, rp, info)
		{
		}

		Hash hash = 0;
		unsigned index = 0;
	};

	Device *device;
	WeakList<FramebufferNode> rings[VULKAN_FRAMEBUFFER_RING_SIZE];
	ObjectPool<FramebufferNode> object_pool;
	unsigned index = 0;
	HashMap<WeakList<FramebufferNode>::Iterator> framebuffers;
};
}
