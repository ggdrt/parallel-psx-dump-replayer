#include "device.hpp"
#include "renderer/renderer.hpp"
#include "stb_image_write.h"

#ifdef VULKAN_WSI
#include "wsi.hpp"
#endif
#include <cmath>
#include <random>
#include <renderer.hpp>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace PSX;
using namespace std;
using namespace Vulkan;

//#define DUMP_VRAM
#define SCALING 4
//#define DETAIL_DUMP_FRAME 153
//#define BREAK_FRAME 40
//#define BREAK_DRAW 216

#define BREAKPOINT __builtin_trap

enum
{
	RSX_END = 0,
	RSX_PREPARE_FRAME,
	RSX_FINALIZE_FRAME,
	RSX_TEX_WINDOW,
	RSX_DRAW_OFFSET,
	RSX_DRAW_AREA,
	RSX_DISPLAY_MODE,
	RSX_TRIANGLE,
	RSX_QUAD,
	RSX_LINE,
	RSX_LOAD_IMAGE,
	RSX_FILL_RECT,
	RSX_COPY_RECT,
	RSX_TOGGLE_DISPLAY
};

static void read_tag(FILE *file)
{
	char buffer[8];
	if (fread(buffer, sizeof(buffer), 1, file) != 1)
		throw runtime_error("Failed to read tag.");
	if (memcmp(buffer, "RSXDUMP2", sizeof(buffer)))
		throw runtime_error("Failed to read tag.");
}

static uint32_t read_u32(FILE *file)
{
	uint32_t val;
	if (fread(&val, sizeof(val), 1, file) != 1)
		throw runtime_error("Failed to read u32");
	return val;
}

static int32_t read_i32(FILE *file)
{
	int32_t val;
	if (fread(&val, sizeof(val), 1, file) != 1)
		throw runtime_error("Failed to read i32");
	return val;
}

static int32_t read_f32(FILE *file)
{
	float val;
	if (fread(&val, sizeof(val), 1, file) != 1)
		throw runtime_error("Failed to read f32");
	return val;
}

struct CommandVertex
{
	float x, y, w;
	uint32_t color;
	uint16_t tx, ty;
};

struct RenderState
{
	uint16_t texpage_x, texpage_y;
	uint16_t clut_x, clut_y;
	uint8_t texture_blend_mode;
	uint8_t depth_shift;
	bool dither;
	uint32_t blend_mode;
	bool mask_test;
	bool set_mask;
};

CommandVertex read_vertex(FILE *file)
{
	CommandVertex buf = {};
	buf.x = read_f32(file);
	buf.y = read_f32(file);
	buf.w = read_f32(file);
	buf.color = read_u32(file);
	buf.tx = uint16_t(read_u32(file));
	buf.ty = uint16_t(read_u32(file));
	return buf;
}

RenderState read_state(FILE *file)
{
	RenderState state = {};
	state.texpage_x = read_u32(file);
	state.texpage_y = read_u32(file);
	state.clut_x = read_u32(file);
	state.clut_y = read_u32(file);
	state.texture_blend_mode = read_u32(file);
	state.depth_shift = read_u32(file);
	state.dither = read_u32(file) != 0;
	state.blend_mode = read_u32(file);
	state.mask_test = read_u32(file) != 0;
	state.set_mask = read_u32(file) != 0;
	return state;
}

struct CommandLine
{
	int16_t x0, y0, x1, y1;
	uint32_t c0, c1;
	bool dither;
	uint32_t blend_mode;
	bool mask_test;
	bool set_mask;
};

CommandLine read_line(FILE *file)
{
	CommandLine line = {};
	line.x0 = read_i32(file);
	line.y0 = read_i32(file);
	line.x1 = read_i32(file);
	line.y1 = read_i32(file);
	line.c0 = read_u32(file);
	line.c1 = read_u32(file);
	line.dither = read_u32(file) != 0;
	line.blend_mode = read_u32(file);
	line.mask_test = read_u32(file) != 0;
	line.set_mask = read_u32(file) != 0;
	return line;
}

static void log_vertex(const CommandVertex &v)
{
	fprintf(stderr, "  x = %.1f, y = %.1f, w = %.1f, c = 0x%x, u = %u, v = %u\n", v.x, v.y, v.w, v.color, v.tx, v.ty);
}

static void log_state(const RenderState &s)
{
	fprintf(
	    stderr,
	    " Page = (%u, %u), CLUT = (%u, %u), texture_blend_mode = %u, depth_shift = %u, dither = %s, blend_mode = %u\n",
	    s.texpage_x, s.texpage_y, s.clut_x, s.clut_y, s.texture_blend_mode, s.depth_shift, s.dither ? "on" : "off",
	    s.blend_mode);
}

static void set_renderer_state(Renderer &renderer, const RenderState &state)
{
	renderer.set_texture_color_modulate(state.texture_blend_mode == 2);
	renderer.set_palette_offset(state.clut_x, state.clut_y);
	renderer.set_texture_offset(state.texpage_x, state.texpage_y);
	renderer.set_dither(state.dither);
	renderer.set_mask_test(state.mask_test);
	renderer.set_force_mask_bit(state.set_mask);
	if (state.texture_blend_mode != 0)
	{
		switch (state.depth_shift)
		{
		default:
		case 0:
			renderer.set_texture_mode(TextureMode::ABGR1555);
			break;
		case 1:
			renderer.set_texture_mode(TextureMode::Palette8bpp);
			break;
		case 2:
			renderer.set_texture_mode(TextureMode::Palette4bpp);
			break;
		}
	}
	else
		renderer.set_texture_mode(TextureMode::None);

	switch (state.blend_mode)
	{
	default:
		renderer.set_semi_transparent(SemiTransparentMode::None);
		break;

	case 0:
		renderer.set_semi_transparent(SemiTransparentMode::Average);
		break;
	case 1:
		renderer.set_semi_transparent(SemiTransparentMode::Add);
		break;
	case 2:
		renderer.set_semi_transparent(SemiTransparentMode::Sub);
		break;
	case 3:
		renderer.set_semi_transparent(SemiTransparentMode::AddQuarter);
		break;
	}
}

static void dump_to_file(Device &device, Renderer &renderer, unsigned index, unsigned subindex)
{
	unsigned width, height;
	//auto buffer = renderer.scanout_to_buffer(true, width, height);
	auto buffer = renderer.scanout_vram_to_buffer(width, height);
	if (!buffer)
		return;

	char path[1024];
	snprintf(path, sizeof(path), "dump/test-%06u-%06u.bmp", index, subindex);

	uint32_t *data = static_cast<uint32_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ));
	for (unsigned i = 0; i < width * height; i++)
		data[i] |= 0xff000000u;

	if (!stbi_write_bmp(path, width, height, 4, data))
		LOG("Failed to write image.");
	device.unmap_host_buffer(*buffer);
}

static void dump_vram_to_file(Device &device, Renderer &renderer, unsigned index)
{
	unsigned width, height;
	auto buffer = renderer.scanout_vram_to_buffer(width, height);
	if (!buffer)
		return;

	char path[1024];
	snprintf(path, sizeof(path), "dump/test-vram-%06u.bmp", index);

	uint32_t *data = static_cast<uint32_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ));
	for (unsigned i = 0; i < width * height; i++)
		data[i] |= 0xff000000u;

	if (!stbi_write_bmp(path, width, height, 4, data))
		LOG("Failed to write image.");
	device.unmap_host_buffer(*buffer);
}

static bool read_command(FILE *file, Device &device, Renderer &renderer, bool &eof, unsigned &frame,
                         unsigned &draw_call)
{
	auto op = read_u32(file);
	eof = false;
	switch (op)
	{
	case RSX_PREPARE_FRAME:
		break;
	case RSX_FINALIZE_FRAME:
		return false;
	case RSX_END:
		eof = true;
		return false;

	case RSX_TEX_WINDOW:
	{
		auto tww = read_u32(file);
		auto twh = read_u32(file);
		auto twx = read_u32(file);
		auto twy = read_u32(file);

		auto tex_x_mask = ~(tww << 3);
		auto tex_y_mask = ~(twh << 3);
		auto tex_x_or = (twx & tww) << 3;
		auto tex_y_or = (twy & twh) << 3;

		renderer.set_texture_window({ uint8_t(tex_x_mask), uint8_t(tex_y_mask), uint8_t(tex_x_or), uint8_t(tex_y_or) });
		break;
	}

	case RSX_DRAW_OFFSET:
	{
		auto x = read_i32(file);
		auto y = read_i32(file);

		renderer.set_draw_offset(x, y);
		break;
	}

	case RSX_DRAW_AREA:
	{
		auto x0 = read_u32(file);
		auto y0 = read_u32(file);
		auto x1 = read_u32(file);
		auto y1 = read_u32(file);

		int width = x1 - x0 + 1;
		int height = y1 - y0 + 1;
		width = max(width, 0);
		height = max(height, 0);

		width = min(width, int(FB_WIDTH - x0));
		height = min(height, int(FB_HEIGHT - y0));
		renderer.set_draw_rect({ x0, y0, unsigned(width), unsigned(height) });
		break;
	}

	case RSX_DISPLAY_MODE:
	{
		auto x = read_u32(file);
		auto y = read_u32(file);
		auto w = read_u32(file);
		auto h = read_u32(file);
		auto depth_24bpp = read_u32(file);

		renderer.set_display_mode({ x, y, w, h }, depth_24bpp != 0);
		break;
	}

	case RSX_TRIANGLE:
	{
		auto v0 = read_vertex(file);
		auto v1 = read_vertex(file);
		auto v2 = read_vertex(file);
		auto state = read_state(file);

		Vertex vertices[3] = {
			{ v0.x, v0.y, v0.w, v0.color, v0.tx, v0.ty },
			{ v1.x, v1.y, v1.w, v1.color, v1.tx, v1.ty },
			{ v2.x, v2.y, v2.w, v2.color, v2.tx, v2.ty },
		};

		set_renderer_state(renderer, state);
#if defined(BREAK_FRAME) && defined(BREAK_DRAW)
		if (frame == BREAK_FRAME && draw_call == BREAK_DRAW)
			BREAKPOINT();
#endif

		renderer.draw_triangle(vertices);

#ifdef DETAIL_DUMP_FRAME
		if (frame == DETAIL_DUMP_FRAME)
			dump_to_file(device, renderer, frame, draw_call);
#endif

		draw_call++;

		break;
	}

	case RSX_QUAD:
	{
		auto v0 = read_vertex(file);
		auto v1 = read_vertex(file);
		auto v2 = read_vertex(file);
		auto v3 = read_vertex(file);
		auto state = read_state(file);

		Vertex vertices[4] = {
			{ v0.x, v0.y, v0.w, v0.color, v0.tx, v0.ty },
			{ v1.x, v1.y, v1.w, v1.color, v1.tx, v1.ty },
			{ v2.x, v2.y, v2.w, v2.color, v2.tx, v2.ty },
			{ v3.x, v3.y, v3.w, v3.color, v3.tx, v3.ty },
		};

		set_renderer_state(renderer, state);
#if defined(BREAK_FRAME) && defined(BREAK_DRAW)
		if (frame == BREAK_FRAME && draw_call == BREAK_DRAW)
			BREAKPOINT();
#endif

		renderer.draw_quad(vertices);

#ifdef DETAIL_DUMP_FRAME
		if (frame == DETAIL_DUMP_FRAME)
			dump_to_file(device, renderer, frame, draw_call);
#endif

		draw_call++;

		break;
	}

	case RSX_LINE:
	{
		auto line = read_line(file);

		Vertex vertices[2] = {
			{ float(line.x0), float(line.y0), 1.0f, line.c0, 0, 0 },
			{ float(line.x1), float(line.y1), 1.0f, line.c1, 0, 0 },
		};

		renderer.set_texture_color_modulate(false);
		renderer.set_texture_mode(TextureMode::None);
		renderer.set_dither(line.dither);
		renderer.set_mask_test(line.mask_test);
		renderer.set_force_mask_bit(line.set_mask);
		switch (line.blend_mode)
		{
		default:
			renderer.set_semi_transparent(SemiTransparentMode::None);
			break;

		case 0:
			renderer.set_semi_transparent(SemiTransparentMode::Average);
			break;
		case 1:
			renderer.set_semi_transparent(SemiTransparentMode::Add);
			break;
		case 2:
			renderer.set_semi_transparent(SemiTransparentMode::Sub);
			break;
		case 3:
			renderer.set_semi_transparent(SemiTransparentMode::AddQuarter);
			break;
		}

#if defined(BREAK_FRAME) && defined(BREAK_DRAW)
		if (frame == BREAK_FRAME && draw_call == BREAK_DRAW)
			BREAKPOINT();
#endif

		renderer.draw_line(vertices);

#ifdef DETAIL_DUMP_FRAME
		if (frame == DETAIL_DUMP_FRAME)
			dump_to_file(device, renderer, frame, draw_call);
#endif

		draw_call++;
		break;
	}

	case RSX_LOAD_IMAGE:
	{
		auto x = read_u32(file);
		auto y = read_u32(file);
		auto width = read_u32(file);
		auto height = read_u32(file);
		bool mask_test = read_u32(file) != 0;
		bool set_mask = read_u32(file) != 0;

		renderer.set_mask_test(mask_test);
		renderer.set_force_mask_bit(set_mask);
		auto handle = renderer.copy_cpu_to_vram({ x, y, width, height });
		uint16_t *ptr = renderer.begin_copy(handle);
		fread(ptr, sizeof(uint16_t), width * height, file);
		renderer.end_copy(handle);
		break;
	}

	case RSX_FILL_RECT:
	{
		auto color = read_u32(file);
		auto x = read_u32(file);
		auto y = read_u32(file);
		auto w = read_u32(file);
		auto h = read_u32(file);

#if defined(BREAK_FRAME) && defined(BREAK_DRAW)
		if (frame == BREAK_FRAME && draw_call == BREAK_DRAW)
			BREAKPOINT();
#endif

		renderer.clear_rect({ x, y, w, h }, color);

#ifdef DETAIL_DUMP_FRAME
		if (frame == DETAIL_DUMP_FRAME)
			dump_to_file(device, renderer, frame, draw_call);
#endif

		draw_call++;

		break;
	}

	case RSX_COPY_RECT:
	{
		auto src_x = read_u32(file);
		auto src_y = read_u32(file);
		auto dst_x = read_u32(file);
		auto dst_y = read_u32(file);
		auto w = read_u32(file);
		auto h = read_u32(file);
		bool mask_test = read_u32(file) != 0;
		bool set_mask = read_u32(file) != 0;
		renderer.set_mask_test(mask_test);
		renderer.set_force_mask_bit(set_mask);
		if (src_x != dst_x || src_y != dst_y)
			renderer.blit_vram({ dst_x, dst_y, w, h }, { src_x, src_y, w, h });
		break;
	}

	case RSX_TOGGLE_DISPLAY:
	{
		auto toggle = read_u32(file);
		renderer.toggle_display(toggle == 0);
		break;
	}

	default:
		throw runtime_error("Invalid opcode.");
	}
	return true;
}

static double gettime()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

int main()
{
	WSI wsi;
	wsi.init(1280, 960);
	auto &device = wsi.get_device();
	Renderer renderer(device, SCALING, nullptr);

#if 1
	FILE *file = fopen("/tmp/crash.rsx", "rb");
	if (!file)
		return 1;

	read_tag(file);
#endif

	bool eof = false;
	unsigned frames = 0;
	unsigned draw_call = 0;
	double total_time = 0.0;
	while (!eof && wsi.alive())
	{
		draw_call = 0;

		double start = gettime();
		wsi.begin_frame();
		renderer.reset_counters();

#if 1
		while (read_command(file, device, renderer, eof, frames, draw_call))
			;
#endif
		renderer.scanout();

#ifdef DUMP_VRAM
		dump_vram_to_file(device, renderer, frames);
#endif

		renderer.flush();
		wsi.end_frame();
		double end = gettime();
		total_time += end - start;
		frames++;

#if 1
		if (renderer.counters.render_passes)
		{
			LOG("========================\n");
			LOG("Completed frame %u.\n", frames);
			LOG("Render passes: %u\n", renderer.counters.render_passes);
			LOG("Draw calls: %u\n", renderer.counters.draw_calls);
			LOG("Texture flushes: %u\n", renderer.counters.texture_flushes);
			LOG("Vertices: %u\n", renderer.counters.vertices);
			LOG("========================\n");
		}
#endif
	}

	LOG("Ran %u frames in %f s! (%.3f ms / frame).\n", frames, total_time, 1000.0 * total_time / frames);
}
