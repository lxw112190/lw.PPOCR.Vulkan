// Real-device tensor gate: exact FP32 bits vs CPU double reference, plus tail canaries.
#include "vulkan_context.hpp"
#include "det_preprocess.hpp"
#ifdef LWVK_TEXT_PREPROCESS_PROBE
#include "cls_preprocess.hpp"
#include "rec_preprocess.hpp"
#endif
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace lwvk;
struct Commands {
    Context& ctx;
    VkCommandPool pool{};
    VkDescriptorPool descriptors{};
    VkFence fence{};
    ~Commands() {
        // Also safe if a submit/wait throws: timeout is not cancellation.
        vkDeviceWaitIdle(ctx.device);
        if (fence)
            vkDestroyFence(ctx.device, fence, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(ctx.device, descriptors, nullptr);
        if (pool)
            vkDestroyCommandPool(ctx.device, pool, nullptr);
    }
};
static void test(Context& ctx, uint32_t w, uint32_t h, uint32_t pad, uint32_t ow, uint32_t oh, bool rotate = false) {
    uint32_t stride = w * 3 + pad;
    size_t span = size_t(h - 1) * stride + w * 3;
    std::vector<uint8_t> pixels(span);
    for (size_t i = 0; i < span; ++i)
        pixels[i] = uint8_t((i * 73 + i / 7) % 256);
#ifdef LWVK_TEXT_PREPROCESS_PROBE
    std::vector<uint8_t> contiguous(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < 3; ++c) {
                const auto dst = rotate ? (size_t(h - 1 - y) * w + w - 1 - x) * 3 + c : (size_t(y) * w + x) * 3 + c;
                contiguous[dst] = pixels[size_t(y) * stride + x * 3 + c];
            }
    auto cpu = oh == 80 ? preprocess_cls_bgr(contiguous.data(), w, h)
                        : preprocess_rec_bgr(contiguous.data(), contiguous.size(), w, h, w * 3, ow).data;
#else
    auto cpu = preprocess_det_bgr(pixels.data(), span, w, h, stride, ow, oh);
#endif
    std::vector<uint32_t> packed(8 + (span + 3) / 4, 0);
    packed[0] = w;
    packed[1] = h;
    packed[2] = stride;
    packed[3] = rotate ? 1u : 0u;
    std::memcpy(packed.data() + 8, pixels.data(), span);
    std::vector<float> output(cpu.size() + 16, -9876.f);
    Buffer input(ctx, packed.size() * 4, true), result(ctx, output.size() * 4, true);
    input.write_parts(packed.data(), 32, pixels.data(), span, (4 - span % 4) % 4);
    // Bound checks must reject before touching the mapped buffer.
    bool rejected = false;
    try {
        input.write_parts(packed.data(), input.size + 1, pixels.data(), span, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("segmented staging bounds ignored");
    result.write(output.data(), output.size() * 4);
    Commands resources{ctx};
#ifdef LWVK_TEXT_PREPROCESS_PROBE
    auto& pipeline = ctx.pipeline("bgr_text_preprocess", 2, 8);
#else
    auto& pipeline = ctx.pipeline("bgr_det_preprocess", 2, 8);
#endif
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = ctx.queue_family;
    check(vkCreateCommandPool(ctx.device, &pool, nullptr, &resources.pool), "probe pool");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    check(vkCreateDescriptorPool(ctx.device, &dp, nullptr, &resources.descriptors), "probe descriptors");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = resources.descriptors;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &pipeline.descriptor_layout;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(ctx.device, &allocation, &set), "probe set");
    std::array<VkDescriptorBufferInfo, 2> buffers{{{input.handle, 0, input.size}, {result.handle, 0, result.size}}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(ctx.device, 2, writes.data(), 0, nullptr);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = resources.pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(vkAllocateCommandBuffers(ctx.device, &ca, &command), "probe command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin), "probe begin");
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0, nullptr);
    const uint32_t push[] = {oh, ow};
    vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push);
    vkCmdDispatch(command, (oh * ow * 3 + 255) / 256, 1, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    check(vkEndCommandBuffer(command), "probe end");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(ctx.device, &fc, nullptr, &resources.fence), "probe fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(ctx.queue, 1, &submit, resources.fence), "probe submit");
    check(vkWaitForFences(ctx.device, 1, &resources.fence, VK_TRUE, UINT64_C(30000000000)), "probe wait");
    result.read(output.data(), output.size() * 4);
    for (uint32_t pixel = 0; pixel < ow * oh; ++pixel)
        for (uint32_t c = 0; c < 3; ++c) {
            const float expected = cpu[size_t(c) * ow * oh + pixel];
            if (std::memcmp(&expected, &output[pixel * 3 + c], 4))
                throw std::runtime_error("FP32 tensor bits differ at pixel " + std::to_string(pixel));
        }
    for (size_t i = cpu.size(); i < output.size(); ++i)
        if (output[i] != -9876.f)
            throw std::runtime_error("output tail overwritten");
}
int main(int argc, char** argv) {
    try {
        Context ctx(argc > 1 ? uint32_t(std::stoul(argv[1])) : 0);
#ifdef LWVK_TEXT_PREPROCESS_PROBE
        if (!ctx.gpu_text_preprocess)
            throw std::runtime_error("set LWVK_GPU_TEXT_PREPROCESS=1 before launch");
        const std::array<std::array<uint32_t, 5>, 10> cases{{{1, 1, 0, 32, 48},
                                                             {2, 3, 1, 32, 48},
                                                             {17, 11, 5, 80, 48},
                                                             {501, 23, 7, 960, 48},
                                                             {12, 96, 3, 32, 48},
                                                             {960, 48, 0, 960, 48},
                                                             {1, 1, 0, 160, 80},
                                                             {1000, 24, 7, 160, 80},
                                                             {97, 25, 5, 160, 80},
                                                             {24, 160, 1, 160, 80}}};
        for (const auto& c : cases) {
            test(ctx, c[0], c[1], c[2], c[3], c[4]);
            if (c[4] == 48)
                test(ctx, c[0], c[1], c[2], c[3], c[4], true);
        }
        std::cout << "PASS: 16 GPU CLS/REC tensor cases, rotation/padding/stride/exact FP32 bits/canaries; "
                  << ctx.properties.deviceName << '\n';
#else
        if (!ctx.gpu_det_preprocess)
            throw std::runtime_error("set LWVK_GPU_DET_PREPROCESS=1 before launch");
        const std::array<std::array<uint32_t, 5>, 8> cases{{{1, 1, 0, 32, 32},
                                                            {2, 3, 1, 64, 32},
                                                            {17, 11, 5, 32, 32},
                                                            {500, 500, 0, 512, 512},
                                                            {501, 503, 7, 512, 512},
                                                            {1280, 720, 3, 960, 544},
                                                            {2448, 3264, 5, 736, 960},
                                                            {960, 960, 0, 960, 960}}};
        for (const auto& c : cases)
            test(ctx, c[0], c[1], c[2], c[3], c[4]);
        std::cout << "PASS: 8 GPU DET tensor cases, exact FP32 bits and tail canaries; " << ctx.properties.deviceName
                  << '\n';
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
