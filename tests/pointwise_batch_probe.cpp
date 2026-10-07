// Exact FP32/arena guard gate for virtual concatenation of independent lines.
#include "vulkan_context.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace lwvk;
static void run_case(Context& ctx, std::array<uint32_t, 4> rows, uint32_t slots, uint32_t n, uint32_t k,
                     uint32_t flags) {
    const uint32_t align =
        static_cast<uint32_t>(std::max<VkDeviceSize>(16, ctx.properties.limits.minStorageBufferOffsetAlignment) / 4);
    auto round = [&](uint32_t value) { return (value + align - 1) / align * align; };
    std::array<uint32_t, 16> push{n, k, flags, slots};
    uint32_t size = align;
    for (uint32_t i = 0; i < slots; ++i) {
        push[4 + i] = rows[i];
        push[8 + i] = size;
        size = round(size + rows[i] * k + 16);
        push[12 + i] = size;
        size = round(size + rows[i] * n + 16);
    }
    std::vector<float> initial(size, -9876.0f), weights(k * n), bias(n);
    for (uint32_t s = 0; s < slots; ++s)
        for (uint32_t i = 0; i < rows[s] * k; ++i)
            initial[push[8 + s] + i] = float(int((i * 7 + s * 19) % 61) - 30) / 64;
    for (uint32_t i = 0; i < k * n; ++i)
        weights[i] = float(int(i * 13 % 57) - 28) / 128;
    for (uint32_t i = 0; i < n; ++i)
        bias[i] = float(i % 11) / 256;
    Buffer a(ctx, size * 4, true), b(ctx, size * 4, true), w(ctx, weights.size() * 4, true),
        bias_buffer(ctx, bias.size() * 4, true);
    a.write(initial.data(), initial.size() * 4);
    b.write(initial.data(), initial.size() * 4);
    w.write(weights.data(), weights.size() * 4);
    bias_buffer.write(bias.data(), bias.size() * 4);
    VkCommandPool pool{};
    VkDescriptorPool descriptors{};
    VkFence fence{};
    auto close = [&] {
        if (fence)
            vkDestroyFence(ctx.device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(ctx.device, pool, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(ctx.device, descriptors, nullptr);
    };
    try {
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = ctx.queue_family;
        check(vkCreateCommandPool(ctx.device, &pc, nullptr, &pool), "batch probe pool");
        VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (slots + 1) * 4};
        VkDescriptorPoolCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dc.maxSets = slots + 1;
        dc.poolSizeCount = 1;
        dc.pPoolSizes = &sz;
        check(vkCreateDescriptorPool(ctx.device, &dc, nullptr, &descriptors), "batch probe descriptors");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(vkAllocateCommandBuffers(ctx.device, &ai, &command), "batch probe command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "batch probe begin");
        auto barrier = [&](VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags source,
                           VkAccessFlags target) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = source;
            mb.dstAccessMask = target;
            vkCmdPipelineBarrier(command, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
        };
        barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        auto dispatch = [&](const char* name, const std::array<VkDescriptorBufferInfo, 4>& bindings,
                            const uint32_t* constants, uint32_t bytes, uint32_t m) {
            auto& p = ctx.pipeline(name, 4, bytes);
            VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ds.descriptorPool = descriptors;
            ds.descriptorSetCount = 1;
            ds.pSetLayouts = &p.descriptor_layout;
            VkDescriptorSet set{};
            check(vkAllocateDescriptorSets(ctx.device, &ds, &set), "batch probe set");
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t i = 0; i < 4; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &bindings[i];
            }
            vkUpdateDescriptorSets(ctx.device, 4, writes.data(), 0, nullptr);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.handle);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(command, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, constants);
            vkCmdDispatch(command, (m + 31) / 32, (n + 127) / 128, 1);
        };
        for (uint32_t i = 0; i < slots; ++i) {
            const std::array<VkDescriptorBufferInfo, 4> bindings{{{a.handle, push[8 + i] * 4, rows[i] * k * 4},
                                                                  {w.handle, 0, w.size},
                                                                  {bias_buffer.handle, 0, bias_buffer.size},
                                                                  {a.handle, push[12 + i] * 4, rows[i] * n * 4}}};
            const std::array<uint32_t, 4> p{rows[i], n, k, flags};
            dispatch("conv_pointwise_wide", bindings, p.data(), 16, rows[i]);
        }
        const std::array<VkDescriptorBufferInfo, 4> bindings{{{b.handle, 0, b.size},
                                                              {w.handle, 0, w.size},
                                                              {bias_buffer.handle, 0, bias_buffer.size},
                                                              {b.handle, 0, b.size}}};
        dispatch("conv_pointwise_batch", bindings, push.data(), 64,
                 std::accumulate(rows.begin(), rows.begin() + slots, 0u));
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(command), "batch probe end");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(ctx.device, &fc, nullptr, &fence), "batch probe fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(ctx.queue, 1, &submit, fence), "batch probe submit");
        check(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_C(30000000000)), "batch probe wait");
        std::vector<float> baseline(size), actual(size);
        a.read(baseline.data(), size * 4);
        b.read(actual.data(), size * 4);
        if (std::memcmp(actual.data(), baseline.data(), size * 4))
            throw std::runtime_error("merged FP32 bits differ");
        for (uint32_t i = 0; i < size; ++i) {
            bool output = false;
            for (uint32_t s = 0; s < slots; ++s)
                output |= i >= push[12 + s] && i < push[12 + s] + rows[s] * n;
            if (!output && actual[i] != initial[i])
                throw std::runtime_error("arena input/guard overwritten");
        }
        close();
    } catch (...) {
        vkDeviceWaitIdle(ctx.device);
        close();
        throw;
    }
}
int main(int argc, char** argv) {
    try {
        Context ctx(argc > 1 ? static_cast<uint32_t>(std::stoul(argv[1])) : 0);
        const bool quick = argc > 2 && std::string(argv[2]) == "--quick";
        const std::vector<std::array<uint32_t, 2>> nk =
            quick ? std::vector<std::array<uint32_t, 2>>{{68, 36}}
                  : std::vector<std::array<uint32_t, 2>>{{68, 36}, {768, 1536}, {1536, 768}};
        unsigned cases = 0;
        for (uint32_t slots : {2u, 3u, 4u})
            for (auto shape : nk)
                for (uint32_t flags : {0u, 1u, 16u, 17u, 32u, 33u, 128u, 129u, 256u, 257u, 273u}) {
                    run_case(ctx, {1, 17, 31, 65}, slots, shape[0], shape[1], flags);
                    ++cases;
                }
        std::cout << "PASS: " << cases
                  << " merged FP32 cases, exact wide-kernel bits, 2..4 unequal rows and arena canaries; "
                  << ctx.properties.deviceName << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
