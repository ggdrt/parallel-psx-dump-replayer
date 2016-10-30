#pragma once

#include "atlas.hpp"
#include "device.hpp"
#include "vulkan.hpp"
#include "wsi.hpp"
#include "texture.hpp"

namespace PSX
{

struct Vertex
{
	float x, y, w;
	uint32_t color;
	uint8_t u, v;
};

enum class SemiTransparentMode
{
	None,
	Average,
	Add,
	Sub,
	AddQuarter
};

class Renderer : private HazardListener
{
public:
	Renderer(Vulkan::Device &device, unsigned scaling);
	~Renderer();

	void set_draw_rect(const Rect &rect);
	inline void set_draw_offset(int x, int y)
	{
		render_state.draw_offset_x = x;
		render_state.draw_offset_y = y;
	}

	void set_texture_window(const Rect &rect);
	inline void set_texture_offset(unsigned x, unsigned y)
	{
		atlas.set_texture_offset(x, y);
	}

	inline void set_palette_offset(unsigned x, unsigned y)
	{
		atlas.set_palette_offset(x, y);
		render_state.palette_offset_x = x;
		render_state.palette_offset_y = y;
	}

	void copy_cpu_to_vram(const uint16_t *data, const Rect &rect);
	void blit_vram(const Rect &dst, const Rect &src);

	void scanout(const Rect &rect);

	inline void set_texture_mode(TextureMode mode)
	{
		render_state.texture_mode = mode;
		atlas.set_texture_mode(mode);
		allocator.set_texture_mode(mode);
	}

	inline void set_semi_transparent(SemiTransparentMode state)
	{
		render_state.semi_transparent = state;
	}

	inline void set_force_mask_bit(bool enable)
	{
		render_state.force_mask_bit = enable;
	}

	inline void set_mask_test(bool enable)
	{
		render_state.mask_test = enable;
	}

	inline void set_texture_color_modulate(bool enable)
	{
		render_state.texture_color_modulate = enable;
	}

	// Draw commands
	void clear_rect(const Rect &rect, FBColor color);
	void draw_triangle(const Vertex *vertices);
	void draw_quad(const Vertex *vertices);

private:
	Vulkan::Device &device;
	unsigned scaling;
	Vulkan::ImageHandle scaled_framebuffer;
	Vulkan::ImageHandle framebuffer;
	Vulkan::ImageHandle depth;
	FBAtlas atlas;

	Vulkan::CommandBufferHandle cmd;

	void hazard(StatusFlags flags) override;
	void resolve(Domain target_domain, const Rect &rect) override;
	void flush_render_pass(const Rect &rect) override;
	void discard_render_pass() override;
	void upload_texture(Domain target_domain, const Rect &rect, unsigned off_x, unsigned off_y) override;
	void clear_quad(const Rect &rect, FBColor color) override;

	struct
	{
		Vulkan::ProgramHandle copy_to_vram;
		Vulkan::ProgramHandle unscaled_quad_blitter;
		Vulkan::ProgramHandle scaled_quad_blitter;
		Vulkan::ProgramHandle resolve_to_scaled;
		Vulkan::ProgramHandle resolve_to_unscaled;
		Vulkan::ProgramHandle blit_vram_unscaled;
		Vulkan::ProgramHandle blit_vram_scaled;
		Vulkan::ProgramHandle opaque_flat;
		Vulkan::ProgramHandle opaque_textured;
		Vulkan::ProgramHandle opaque_semi_transparent;
		Vulkan::ProgramHandle semi_transparent;
		Vulkan::ProgramHandle semi_transparent_masked;
	} pipelines;

	void init_pipelines();
	void ensure_command_buffer();

	struct
	{
		int draw_offset_x = 0;
		int draw_offset_y = 0;
		unsigned palette_offset_x = 0;
		unsigned palette_offset_y = 0;

		TextureMode texture_mode = TextureMode::None;
		SemiTransparentMode semi_transparent = SemiTransparentMode::None;
		bool force_mask_bit = false;
		bool texture_color_modulate = false;
		bool mask_test = false;
	} render_state;

	struct BufferVertex
	{
		float x, y, z, w;
		float u, v, layer;
		uint32_t color;
	};

	struct SemiTransparentState
	{
		unsigned image_index;
		SemiTransparentMode semi_transparent;
		bool textured;
		bool masked;

		bool operator==(const SemiTransparentState &other) const
		{
			return image_index == other.image_index &&
				semi_transparent == other.semi_transparent &&
				textured == other.textured &&
				masked == other.masked;
		}

		bool operator!=(const SemiTransparentState &other) const
		{
			return !(*this == other);
		}
	};

	struct OpaqueQueue
	{
		// Non-textured primitives.
		std::vector<BufferVertex> opaque;

		// Textured primitives, no semi-transparency.
		std::vector<std::vector<BufferVertex>> opaque_textured;

		// Textured primitives, semi-transparency enabled.
		std::vector<std::vector<BufferVertex>> semi_transparent_opaque;

		std::vector<BufferVertex> semi_transparent;
		std::vector<SemiTransparentState> semi_transparent_state;

		std::vector<Vulkan::ImageHandle> textures;
	} queue;
	unsigned primitive_index = 0;
	TextureSurface last_surface;
	float last_uv_scale_x, last_uv_scale_y;

	void render_opaque_primitives();
	void render_opaque_texture_primitives();
	void render_semi_transparent_opaque_texture_primitives();
	void render_semi_transparent_primitives();
	void reset_queue();

	float allocate_depth();
	void flush_texture_allocator();
	TextureAllocator allocator;

	void build_attribs(BufferVertex *verts, const Vertex *vertices, unsigned count);
	std::vector<BufferVertex> *select_pipeline();
};
}