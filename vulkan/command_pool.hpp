#pragma once

#include <vector>
#include "vulkan.hpp"

namespace Vulkan
{
class CommandPool
{
   public:
      CommandPool(VkDevice device);
      ~CommandPool();

      void begin();
      VkCommandBuffer request_command_buffer();

   private:
      VkDevice device;
      VkCommandPool pool;
      std::vector<VkCommandBuffer> buffers;
      unsigned index = 0;
};
}
