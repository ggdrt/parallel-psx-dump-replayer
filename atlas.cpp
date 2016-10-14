#include "atlas.hpp"
#include <stdio.h>
#include <algorithm>

using namespace std;

namespace PSX
{

FBAtlas::FBAtlas()
{
   fb_info.resize(NUM_BLOCKS_X * NUM_BLOCKS_Y);
   for (auto &f : fb_info)
      f = STATUS_FB_PREFER;
}

void FBAtlas::read_compute(Domain domain, const Rect &rect)
{
   sync_domain(domain, rect);
   read_domain(domain, Stage::Compute, rect);
}

void FBAtlas::write_compute(Domain domain, const Rect &rect)
{
   sync_domain(domain, rect);
   write_domain(domain, Stage::Compute, rect);
}

void FBAtlas::read_transfer(Domain domain, const Rect &rect)
{
   sync_domain(domain, rect);
   read_domain(domain, Stage::Transfer, rect);
}

void FBAtlas::write_transfer(Domain domain, const Rect &rect)
{
   sync_domain(domain, rect);
   write_domain(domain, Stage::Transfer, rect);
}

void FBAtlas::read_texture(const Rect &rect)
{
   auto domain = find_suitable_domain(rect);
   sync_domain(domain, rect);
   read_domain(domain, Stage::Compute, rect);
}

void FBAtlas::write_domain(Domain domain, Stage stage, const Rect &rect)
{
   if (inside_render_pass(rect))
      flush_render_pass();

   unsigned xbegin = rect.x / BLOCK_WIDTH;
   unsigned xend = (rect.x + rect.width - 1) / BLOCK_WIDTH;
   unsigned ybegin = rect.y / BLOCK_HEIGHT;
   unsigned yend = (rect.y + rect.height - 1) / BLOCK_HEIGHT;

   unsigned write_domains = 0;
   unsigned hazard_domains;
   unsigned resolve_domains;
   if (domain == Domain::Unscaled)
   {
      hazard_domains = STATUS_FB_WRITE | STATUS_FB_READ;
      if (stage == Stage::Compute)
         resolve_domains = STATUS_COMPUTE_FB_WRITE | STATUS_FB_ONLY;
      else if (stage == Stage::Transfer)
         resolve_domains = STATUS_TRANSFER_FB_WRITE | STATUS_FB_ONLY;
   }
   else
   {
      hazard_domains = STATUS_SFB_WRITE | STATUS_SFB_READ;
      if (stage == Stage::Compute)
         resolve_domains = STATUS_COMPUTE_SFB_WRITE;
      else if (stage == Stage::Fragment)
      {
         hazard_domains &= ~(STATUS_FRAGMENT_SFB_WRITE | STATUS_FRAGMENT_SFB_READ);
         resolve_domains = STATUS_FRAGMENT_SFB_WRITE;
      }
      else if (stage == Stage::Transfer)
         resolve_domains = STATUS_TRANSFER_SFB_WRITE;
      resolve_domains |= STATUS_SFB_ONLY;
   }

   for (unsigned y = ybegin; y <= yend; y++)
      for (unsigned x = xbegin; x <= xend; x++)
         write_domains |= info(x, y) & hazard_domains;

   if (write_domains)
      pipeline_barrier(write_domains);

   for (unsigned y = ybegin; y <= yend; y++)
      for (unsigned x = xbegin; x <= xend; x++)
         info(x, y) = (info(x, y) & ~STATUS_OWNERSHIP_MASK) | resolve_domains;
}

void FBAtlas::read_domain(Domain domain, Stage stage,
      const Rect &rect)
{
   if (inside_render_pass(rect))
      flush_render_pass();

   unsigned xbegin = rect.x / BLOCK_WIDTH;
   unsigned xend = (rect.x + rect.width - 1) / BLOCK_WIDTH;
   unsigned ybegin = rect.y / BLOCK_HEIGHT;
   unsigned yend = (rect.y + rect.height - 1) / BLOCK_HEIGHT;

   unsigned write_domains = 0;
   unsigned hazard_domains;
   unsigned resolve_domains;
   if (domain == Domain::Unscaled)
   {
      hazard_domains = STATUS_FB_WRITE;
      if (stage == Stage::Compute)
         resolve_domains = STATUS_COMPUTE_FB_READ;
      else if (stage == Stage::Transfer)
         resolve_domains = STATUS_TRANSFER_FB_READ;
   }
   else
   {
      hazard_domains = STATUS_SFB_WRITE;
      if (stage == Stage::Compute)
         resolve_domains = STATUS_COMPUTE_SFB_READ;
      else if (stage == Stage::Transfer)
         resolve_domains = STATUS_TRANSFER_SFB_READ;
      else if (stage == Stage::Fragment)
      {
         hazard_domains &= ~(STATUS_FRAGMENT_SFB_WRITE | STATUS_FRAGMENT_SFB_READ);
         resolve_domains = STATUS_FRAGMENT_SFB_READ;
      }
   }

   for (unsigned y = ybegin; y <= yend; y++)
      for (unsigned x = xbegin; x <= xend; x++)
         write_domains |= info(x, y) & hazard_domains;

   if (write_domains)
      pipeline_barrier(write_domains);

   for (unsigned y = ybegin; y <= yend; y++)
      for (unsigned x = xbegin; x <= xend; x++)
         info(x, y) |= resolve_domains;
}

void FBAtlas::sync_domain(Domain domain, const Rect &rect)
{
   unsigned xbegin = rect.x / BLOCK_WIDTH;
   unsigned xend = (rect.x + rect.width - 1) / BLOCK_WIDTH;
   unsigned ybegin = rect.y / BLOCK_HEIGHT;
   unsigned yend = (rect.y + rect.height - 1) / BLOCK_HEIGHT;

   // If we need to see a "clean" version
   // of a framebuffer domain, we need to see
   // anything other than this flag.
   unsigned dirty_bits =
      1u << (domain == Domain::Unscaled ? STATUS_SFB_ONLY : STATUS_FB_ONLY);
   unsigned bits = 0;

   for (unsigned y = ybegin; y <= yend; y++)
      for (unsigned x = xbegin; x <= xend; x++)
         bits |= 1u << (info(x, y) & STATUS_OWNERSHIP_MASK);

   unsigned write_domains = 0;

   // We're asserting that a region is up to date, but it's
   // not, so we have to resolve it.
   if ((bits & dirty_bits) == 0)
      return;

   if (inside_render_pass(rect))
      flush_render_pass();

   // For scaled domain,
   // we need to blit from unscaled domain to scaled.
   unsigned ownership;
   unsigned hazard_domains;
   unsigned resolve_domains;
   if (domain == Domain::Scaled)
   {
      ownership = STATUS_FB_ONLY;
      hazard_domains = 
         STATUS_FB_WRITE |
         STATUS_SFB_WRITE |
         STATUS_SFB_READ;

      resolve_domains =
         STATUS_TRANSFER_FB_READ |
         STATUS_FB_PREFER |
         STATUS_TRANSFER_SFB_WRITE;
   }
   else
   {
      ownership = STATUS_SFB_ONLY;
      hazard_domains = 
         STATUS_FB_WRITE |
         STATUS_SFB_WRITE |
         STATUS_FB_READ;

      resolve_domains =
         STATUS_TRANSFER_SFB_READ |
         STATUS_SFB_PREFER |
         STATUS_TRANSFER_FB_WRITE;
   }

   for (unsigned y = ybegin; y <= yend; y++)
   {
      for (unsigned x = xbegin; x <= xend; x++)
      {
         auto &mask = info(x, y);
         // If our block isn't in the ownership class we want,
         // we need to read from one block and write to the other.
         // We might have to wait for writers on read,
         // and add hazard masks for our writes
         // so other readers can wait for us.
         if ((mask & STATUS_OWNERSHIP_MASK) == ownership)
            write_domains |= mask & hazard_domains;
      }
   }

   // If we hit any hazard, resolve it.
   if (write_domains)
      pipeline_barrier(write_domains);

   for (unsigned y = ybegin; y <= yend; y++)
   {
      for (unsigned x = xbegin; x <= xend; x++)
      {
         auto &mask = info(x, y);
         if ((mask & STATUS_OWNERSHIP_MASK) == ownership)
         {
            mask &= ~STATUS_OWNERSHIP_MASK;
            mask |= resolve_domains;
         }
      }
   }
}

Domain FBAtlas::find_suitable_domain(const Rect &rect)
{
   unsigned xbegin = rect.x / BLOCK_WIDTH;
   unsigned xend = (rect.x + rect.width - 1) / BLOCK_WIDTH;
   unsigned ybegin = rect.y / BLOCK_HEIGHT;
   unsigned yend = (rect.y + rect.height - 1) / BLOCK_HEIGHT;

   for (unsigned y = ybegin; y <= yend; y++)
   {
      for (unsigned x = xbegin; x <= xend; x++)
      {
         unsigned i = info(x, y);
         if (i == STATUS_FB_ONLY || i == STATUS_FB_PREFER)
            return Domain::Unscaled;
      }
   }
   return Domain::Scaled;
}

bool FBAtlas::inside_render_pass(const Rect &rect)
{
   if (!renderpass.inside)
      return false;

   unsigned xbegin = rect.x & ~(BLOCK_WIDTH - 1);
   unsigned ybegin = rect.y & ~(BLOCK_HEIGHT - 1);
   unsigned xend = ((rect.x + rect.width - 1) | (BLOCK_WIDTH - 1)) + 1;
   unsigned yend = ((rect.y + rect.height - 1) | (BLOCK_HEIGHT - 1)) + 1;

   unsigned x0 = max(rect.x, xbegin);
   unsigned x1 = min(rect.x + rect.width, xend);
   unsigned y0 = max(rect.y, ybegin);
   unsigned y1 = min(rect.y + rect.height, yend);

   return x1 > x0 && y1 > y0;
}

void FBAtlas::flush_render_pass()
{
   if (!renderpass.inside)
      return;

   renderpass.inside = false;
   write_domain(Domain::Scaled, Stage::Fragment, renderpass.rect);
}

void FBAtlas::set_texture_window(const Rect &rect)
{
   renderpass.texture_window = rect;
}

void FBAtlas::write_fragment()
{
   if (inside_render_pass(renderpass.texture_window))
      flush_render_pass();
   read_texture(renderpass.texture_window);

   if (!renderpass.inside)
   {
      sync_domain(Domain::Scaled, renderpass.rect);
      renderpass.inside = true;
      renderpass.clean_clear = false;
      renderpass.wait_for_blit = false;
   }
}

void FBAtlas::clear_rect(const Rect &rect)
{
   if (renderpass.rect == rect)
   {
      sync_domain(Domain::Scaled, rect);

      discard_render_pass();
      renderpass.inside = true;
      renderpass.clean_clear = true;
      renderpass.wait_for_blit = false;
   }
   else if (!renderpass.inside)
   {
      sync_domain(Domain::Scaled, rect);
      renderpass.inside = true;
      renderpass.clean_clear = false;
      renderpass.wait_for_blit = false;
   }
}

void FBAtlas::set_draw_rect(const Rect &rect)
{
   if (!renderpass.inside)
      renderpass.rect = rect;
   else if (renderpass.rect != rect)
      flush_render_pass();
}

void FBAtlas::discard_render_pass()
{
   renderpass.inside = false;
}

void FBAtlas::pipeline_barrier(unsigned domains)
{
   unsigned compute_stages =
      STATUS_COMPUTE_FB_READ |
      STATUS_COMPUTE_FB_WRITE |
      STATUS_COMPUTE_SFB_READ |
      STATUS_COMPUTE_SFB_WRITE;

   unsigned transfer_stages =
      STATUS_TRANSFER_FB_READ |
      STATUS_TRANSFER_SFB_READ |
      STATUS_TRANSFER_FB_WRITE |
      STATUS_TRANSFER_SFB_WRITE;

   unsigned fragment_stages =
      STATUS_FRAGMENT_SFB_WRITE;

   if (domains & compute_stages)
      fprintf(stderr, "Wait for compute\n");
   if (domains & transfer_stages)
      fprintf(stderr, "Wait for transfer\n");
   if (domains & fragment_stages)
      fprintf(stderr, "Wait for fragment\n");

   for (auto &f : fb_info)
      f &= ~domains;
}

}
