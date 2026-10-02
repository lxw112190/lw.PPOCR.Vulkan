// Engineering-only device test. Compare device-local BGR8 crops with the CPU
// reference; explicit transfer/host barriers and tail high-water protection.
#include "gpu_crop.hpp"
#include "crop_optimized.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace lwvk;
static std::vector<uint8_t> read_gpu(Context& ctx, Buffer& gpu) {
    Buffer cpu(ctx, gpu.size, true, true);
    VkCommandPool pool{};
    VkFence fence{};
    auto close = [&] {
        vkDeviceWaitIdle(ctx.device);
        if (fence)
            vkDestroyFence(ctx.device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(ctx.device, pool, nullptr);
    };
    try {
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = ctx.queue_family;
        check(vkCreateCommandPool(ctx.device, &pc, nullptr, &pool), "probe pool");
        VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ac.commandPool = pool;
        ac.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ac.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        check(vkAllocateCommandBuffers(ctx.device, &ac, &cmd), "probe command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin), "probe begin");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
        VkBufferCopy copy{0, 0, gpu.size};
        vkCmdCopyBuffer(cmd, gpu.handle, cpu.handle, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd), "probe end");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(ctx.device, &fc, nullptr, &fence), "probe fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(ctx.queue, 1, &submit, fence), "probe submit");
        check(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_C(30000000000)), "probe wait");
        std::vector<uint8_t> out(gpu.size);
        cpu.read(out.data(), out.size());
        close();
        return out;
    } catch (...) {
        close();
        throw;
    }
}
static lw_detection_box box(std::array<float, 8> v) {
    lw_detection_box b{};
    b.x1 = v[0];
    b.y1 = v[1];
    b.x2 = v[2];
    b.y2 = v[3];
    b.x3 = v[4];
    b.y3 = v[5];
    b.x4 = v[6];
    b.y4 = v[7];
    return b;
}
int main(int argc, char** argv) {
    try {
        auto ctx = std::make_shared<Context>(argc > 1 ? uint32_t(std::stoul(argv[1])) : 1);
        uint64_t reserved = 0;
        GpuCropBatch crops(ctx, 64ull * 1024 * 1024, [&](uint64_t n, const std::vector<Buffer*>&) { reserved = n; });
        unsigned count = 0;
        for (uint32_t pad : {0u, 1u, 7u}) {
            uint32_t w = 83, h = 65, stride = w * 3 + pad;
            std::vector<uint8_t> pixels(size_t(h - 1) * stride + w * 3);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = uint8_t((i * 73 + i / 7) % 256);
            crops.upload({pixels.data(), pixels.size(), w, h, stride});
            std::vector<lw_detection_box> quads{
                box({0, 0, 82, 0, 82, 64, 0, 64}),     box({1.25f, 2.5f, 60.25f, 3.75f, 62.125f, 28.5f, 0.5f, 27.25f}),
                box({10, 1, 26, 3, 29, 58, 9, 61}),    box({-7, -5, 30, -5, 30, 24, -7, 24}),
                box({70, 52, 95, 53, 94, 79, 72, 81}), box({40, 4, 63, 23, 45, 45, 20, 26}),
                box({0, 0, 1, 0, 1, 1, 0, 1}),         box({20, 5, 27, 5, 27, 43, 20, 43})};
            std::vector<std::vector<uint8_t>> previous(8);
            for (unsigned n = 8; n >= 1; --n) {
                std::vector<lw_detection_box> batch(quads.begin(), quads.begin() + n);
                std::vector<BgrView> views;
                crops.crop(batch, views);
                for (size_t i = 0; i < views.size(); ++i) {
                    uint32_t cw = 0, ch = 0;
                    uint64_t bytes = 0;
                    if (lw_crop_quad_size(&batch[i], &cw, &ch, &bytes) != LW_STATUS_OK)
                        throw std::runtime_error("CPU crop size");
                    std::vector<uint8_t> cpu(bytes);
                    if (lwvk_crop_quad_bgr_u8(pixels.data(), pixels.size(), w, h, stride, &batch[i], cpu.data(),
                                              cpu.size(), &cw, &ch, &bytes) != LW_STATUS_OK)
                        throw std::runtime_error("CPU crop reference");
                    auto gpu = read_gpu(*ctx, *views[i].gpu_buffer);
                    if (views[i].width != cw || views[i].height != ch ||
                        std::memcmp(gpu.data() + 32, cpu.data(), cpu.size()))
                        throw std::runtime_error("GPU crop BGR8 mismatch, case " + std::to_string(count));
                    uint32_t header[8]{};
                    std::memcpy(header, gpu.data(), 32);
                    if (header[0] != cw || header[1] != ch || header[2] != cw * 3 || header[3])
                        throw std::runtime_error("GPU crop header");
                    auto needed = 32 + (bytes + 3) / 4 * 4;
                    for (size_t j = 32 + bytes; j < needed; ++j)
                        if (gpu[j])
                            throw std::runtime_error("GPU crop padding");
                    if (previous[i].size() == gpu.size() &&
                        std::memcmp(gpu.data() + needed, previous[i].data() + needed, gpu.size() - needed))
                        throw std::runtime_error("GPU crop tail changed");
                    previous[i] = std::move(gpu);
                    ++count;
                }
            }
            std::vector<BgrView> views;
            crops.crop({quads[0]}, views);
            auto large = read_gpu(*ctx, *views[0].gpu_buffer);
            crops.crop({quads[6]}, views);
            auto small = read_gpu(*ctx, *views[0].gpu_buffer);
            if (large.size() != small.size() || std::memcmp(large.data() + 36, small.data() + 36, large.size() - 36))
                throw std::runtime_error("large-to-small crop tail changed");
            ++count;
            bool rejected = false;
            try {
                crops.crop({box({0, 0, 0, 0, 0, 0, 0, 0})}, views);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected || !views.empty())
                throw std::runtime_error("invalid crop not rejected cleanly");
            crops.crop({quads[0]}, views);
        }
        std::cout << "{\"passed\":true,\"device\":\"" << ctx->properties.deviceName << "\",\"cases\":" << count
                  << ",\"crop_pixels_exact\":true,\"invalid_recovery\":true,\"reserved_bytes\":" << reserved << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
