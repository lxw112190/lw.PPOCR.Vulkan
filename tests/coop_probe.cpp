// Experimental GPU capability + tail/stride/padding correctness gate.
#include "vulkan_context.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>
using namespace lwvk;
#ifdef LWVK_WIDE_POINTWISE_PROBE
constexpr bool wide_pointwise_probe = true;
#else
constexpr bool wide_pointwise_probe = false;
#endif
#ifdef LWVK_VECTOR_GEMM_PROBE
constexpr bool vector_gemm_probe = true;
#else
constexpr bool vector_gemm_probe = false;
#endif
#ifdef LWVK_DEPTHWISE_PROBE
constexpr bool depthwise = true;
#else
constexpr bool depthwise = false;
#endif
#ifdef LWVK_GELU_PROBE
constexpr bool gelu_probe = true;
#else
constexpr bool gelu_probe = false;
#endif
static double run_case(Context& ctx, std::array<uint32_t, 13> p, const std::string& shader_override = {},
                       std::vector<float>* actual = nullptr) {
    const uint32_t ow = p[0], oh = p[1], n = p[2], c = p[3], kh = p[4], kw = p[5], iw = p[10], ih = p[11];
    const uint32_t K = (depthwise ? 1 : c) * kh * kw, np = (n + 3) & ~3u;
    std::vector<float> x(iw * ih * c), w(K * np), b(n), reference(ow * oh * n), out(reference.size() + 16, -9876.0f);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = float(int(i * 7 % 61) - 30) / 64;
    for (uint32_t k = 0; k < K; ++k)
        for (uint32_t j = 0; j < n; ++j)
            w[k * np + j] = float(int((k * 13 + j * 11) % 57) - 28) / 128;
    for (uint32_t j = 0; j < n; ++j)
        b[j] = float(j % 11) / 256;
    for (uint32_t m = 0; m < ow * oh; ++m)
        for (uint32_t j = 0; j < n; ++j) {
            float v = (p[12] & 1) ? b[j] : 0;
            for (uint32_t k = 0; k < K; ++k) {
                uint32_t sp = depthwise ? k : k / c, ci = depthwise ? j : k % c;
                int y = int((m / ow) * p[6] + sp / kw) - int(p[8]), xx = int((m % ow) * p[7] + sp % kw) - int(p[9]);
                if (y >= 0 && y < int(ih) && xx >= 0 && xx < int(iw))
                    v += x[(uint32_t(y) * iw + uint32_t(xx)) * c + ci] * w[k * np + j];
            }
            if (p[12] & 16)
                v = std::max(v, 0.0f);
            if (p[12] & 32)
                v = ((std::erf(v / std::sqrt(2.0f)) + 1.0f) * v) * .5f;
            if (p[12] & 128)
                v = v * (1.0f / (1.0f + std::exp(-v)));
            reference[m * n + j] = v;
        }
    auto uploaded = w;
    if (shader_override == "conv_dense" || shader_override == "conv_gemm") {
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t sp = 0; sp < kh * kw; ++sp)
                for (uint32_t ci = 0; ci < c; ++ci)
                    uploaded[(uint64_t(j) * c + ci) * kh * kw + sp] = w[(uint64_t(sp) * c + ci) * np + j];
    }
    Buffer X(ctx, x.size() * 4, true), W(ctx, w.size() * 4, true), B(ctx, b.size() * 4, true),
        O(ctx, out.size() * 4, true);
    X.write(x.data(), x.size() * 4);
    W.write(uploaded.data(), uploaded.size() * 4);
    B.write(b.data(), b.size() * 4);
    O.write(out.data(), out.size() * 4);
    VkCommandPool pool{};
    VkDescriptorPool dp{};
    VkFence fence{};
    auto close = [&] {
        if (fence)
            vkDestroyFence(ctx.device, fence, nullptr);
        if (dp)
            vkDestroyDescriptorPool(ctx.device, dp, nullptr);
        if (pool)
            vkDestroyCommandPool(ctx.device, pool, nullptr);
    };
    try {
        const auto shader = shader_override.empty() ? (depthwise ? "conv_dw4" : "conv_coop_gemm") : shader_override;
        const bool pointwise = shader == "conv_pointwise" || shader == "conv_pointwise_tiled" ||
                               shader == "conv_pointwise_tiled64" || shader == "conv_pointwise_vector" ||
                               shader == "conv_pointwise_smallm" || shader == "conv_pointwise_wide";
        auto& pipeline = ctx.pipeline(shader, 4, depthwise ? 48 : pointwise ? 16 : 52);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = ctx.queue_family;
        check(vkCreateCommandPool(ctx.device, &pc, nullptr, &pool), "probe command pool");
        VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo dc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dc.maxSets = 1;
        dc.poolSizeCount = 1;
        dc.pPoolSizes = &sz;
        check(vkCreateDescriptorPool(ctx.device, &dc, nullptr, &dp), "probe descriptor pool");
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ds.descriptorPool = dp;
        ds.descriptorSetCount = 1;
        ds.pSetLayouts = &pipeline.descriptor_layout;
        VkDescriptorSet set{};
        check(vkAllocateDescriptorSets(ctx.device, &ds, &set), "probe descriptor set");
        std::array<VkDescriptorBufferInfo, 4> buffers{
            {{X.handle, 0, X.size}, {W.handle, 0, W.size}, {B.handle, 0, B.size}, {O.handle, 0, O.size}}};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t i = 0; i < 4; ++i) {
            auto& wr = writes[i];
            wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = set;
            wr.dstBinding = i;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr.descriptorCount = 1;
            wr.pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(ctx.device, 4, writes.data(), 0, nullptr);
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        check(vkAllocateCommandBuffers(ctx.device, &ca, &cmd), "probe command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin), "probe begin");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0, nullptr);
        if (depthwise) {
            const std::array<uint32_t, 12> push{{ow, oh, n, kh, kw, p[6], p[7], p[8], p[9], iw, ih, p[12]}};
            vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 48, push.data());
            vkCmdDispatch(cmd, (ow * oh * (n / 4) + 255) / 256, 1, 1);
        } else if (pointwise) {
            const std::array<uint32_t, 4> push{{ow * oh, n, c, p[12]}};
            vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push.data());
            if (shader == "conv_pointwise_wide")
                vkCmdDispatch(cmd, (ow * oh + 31) / 32, (n + 127) / 128, 1);
            else if (shader == "conv_pointwise_tiled64")
                vkCmdDispatch(cmd, (ow * oh + 63) / 64, (n + 63) / 64, 1);
            else if (shader == "conv_pointwise_smallm")
                vkCmdDispatch(cmd, (ow * oh * n + 63) / 64, 1, 1);
            else if (shader == "conv_pointwise_tiled" || shader == "conv_pointwise_vector" ||
                     shader == "conv_pointwise_smallm")
                vkCmdDispatch(cmd, (ow * oh + 31) / 32, (n + 63) / 64, 1);
            else
                vkCmdDispatch(cmd, (((ow * oh + 3) / 4) * ((n + 3) / 4) + 255) / 256, 1, 1);
        } else {
            vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 52, p.data());
            if (shader == "conv_dense")
                vkCmdDispatch(cmd, (ow * oh * n + 255) / 256, 1, 1);
            else if (shader == "conv_gemm")
                vkCmdDispatch(cmd, ((ow * oh + 15) / 16) * ((n + 15) / 16), 1, 1);
            else if (shader == "conv_gemm_tiled" || shader == "conv_gemm_vector")
                vkCmdDispatch(cmd, (ow * oh + 31) / 32, (n + 63) / 64, 1);
            else
                vkCmdDispatch(cmd, (ow * oh + 63) / 64, (n + 127) / 128, 1);
        }
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd), "probe end");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(ctx.device, &fc, nullptr, &fence), "probe fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(ctx.queue, 1, &submit, fence), "probe submit");
        auto status = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_C(30000000000));
        if (status != VK_SUCCESS) {
            vkDeviceWaitIdle(ctx.device);
            check(status, "probe wait");
        }
        O.read(out.data(), out.size() * 4);
        close();
    } catch (...) {
        vkDeviceWaitIdle(ctx.device);
        close();
        throw;
    }
    double error = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (!std::isfinite(out[i]))
            throw std::runtime_error("nonfinite cooperative output");
        error = std::max(error, double(std::abs(out[i] - reference[i])));
    }
    for (size_t i = reference.size(); i < out.size(); ++i)
        if (out[i] != -9876.0f)
            throw std::runtime_error("cooperative output tail overwrite");
    if (error > 0.0001)
        throw std::runtime_error("cooperative result mismatch: " + std::to_string(error));
    if (actual)
        *actual = std::move(out);
    return error;
}
int main(int argc, char** argv) {
    try {
        uint32_t index = argc > 1 ? static_cast<uint32_t>(std::stoul(argv[1])) : 0;
        Context ctx(index);
        if (!depthwise && !gelu_probe && !vector_gemm_probe && !wide_pointwise_probe && !ctx.cooperative_matrix)
            throw std::runtime_error("set LWVK_EXPERIMENTAL_COOP=1 for the opt-in probe");
        if (wide_pointwise_probe) {
            const std::array<std::array<uint32_t, 13>, 9> cases{{{1, 1, 4, 4, 1, 1, 1, 1, 0, 0, 1, 1, 0},
                                                                 {17, 1, 68, 36, 1, 1, 1, 1, 0, 0, 17, 1, 0},
                                                                 {31, 1, 132, 68, 1, 1, 1, 1, 0, 0, 31, 1, 0},
                                                                 {32, 1, 128, 64, 1, 1, 1, 1, 0, 0, 32, 1, 0},
                                                                 {33, 1, 256, 132, 1, 1, 1, 1, 0, 0, 33, 1, 0},
                                                                 {67, 1, 100, 512, 1, 1, 1, 1, 0, 0, 67, 1, 0},
                                                                 {35, 1, 512, 1024, 1, 1, 1, 1, 0, 0, 35, 1, 0},
                                                                 {41, 1, 768, 1536, 1, 1, 1, 1, 0, 0, 41, 1, 0},
                                                                 {41, 1, 1536, 768, 1, 1, 1, 1, 0, 0, 41, 1, 0}}};
            double error = 0;
            unsigned count = 0;
            for (auto p : cases)
                for (uint32_t flags : {0u, 1u, 16u, 17u, 32u, 33u, 128u, 129u}) {
                    p[12] = flags;
                    std::vector<float> baseline, candidate;
                    error = std::max(error, run_case(ctx, p, "conv_pointwise_vector", &baseline));
                    error = std::max(error, run_case(ctx, p, "conv_pointwise_wide", &candidate));
                    if (baseline.size() != candidate.size() ||
                        std::memcmp(baseline.data(), candidate.data(), baseline.size() * sizeof(float)) != 0)
                        throw std::runtime_error("wide pointwise changed FP32 bits/guard: case " +
                                                 std::to_string(count));
                    ++count;
                }
            std::cout << "{\"device_index\":" << index << ",\"device\":\"" << ctx.properties.deviceName
                      << "\",\"fp32_wide_pointwise\":true,\"exact_baseline_bits\":true,\"cases\":" << count
                      << ",\"max_absolute_error_vs_cpu\":" << std::setprecision(9) << error << "}\n";
            return 0;
        }
        if (vector_gemm_probe) {
            // Cover M/N/K tails, Cin < 32, padding, unequal stride/kernel,
            // and fused activations without requiring OCR model lowering.
            const std::array<std::array<uint32_t, 13>, 8> cases{{{1, 1, 4, 4, 3, 3, 1, 1, 1, 1, 1, 1, 0},
                                                                 {7, 5, 32, 12, 3, 3, 1, 1, 1, 1, 7, 5, 0},
                                                                 {5, 4, 36, 36, 3, 3, 2, 2, 1, 1, 9, 7, 0},
                                                                 {17, 3, 68, 64, 3, 3, 1, 1, 1, 1, 17, 3, 0},
                                                                 {9, 7, 128, 68, 5, 3, 1, 2, 2, 1, 17, 7, 0},
                                                                 {8, 4, 64, 32, 1, 1, 1, 1, 0, 0, 8, 4, 0},
                                                                 {41, 1, 100, 132, 1, 1, 1, 1, 0, 0, 41, 1, 0},
                                                                 {13, 3, 80, 16, 3, 5, 2, 1, 1, 2, 13, 5, 0}}};
            double error = 0;
            unsigned count = 0;
            for (auto p : cases)
                for (uint32_t flags : {0u, 1u, 16u, 17u, 32u, 33u, 128u, 129u}) {
                    p[12] = flags;
                    std::vector<float> baseline, candidate;
                    error = std::max(error, run_case(ctx, p, "conv_gemm_tiled", &baseline));
                    error = std::max(error, run_case(ctx, p, "conv_gemm_vector", &candidate));
                    if (baseline.size() != candidate.size() ||
                        std::memcmp(baseline.data(), candidate.data(), baseline.size() * sizeof(float)) != 0)
                        throw std::runtime_error("vector GEMM changed FP32 bits/guard: case " + std::to_string(count));
                    ++count;
                }
            std::cout << "{\"device_index\":" << index << ",\"device\":\"" << ctx.properties.deviceName
                      << "\",\"fp32_vector_gemm\":true,\"exact_baseline_bits\":true,\"cases\":" << count
                      << ",\"max_absolute_error_vs_cpu\":" << std::setprecision(9) << error << "}\n";
            return 0;
        }
        if (gelu_probe) {
            const std::array<std::array<uint32_t, 13>, 4> cases{{{41, 1, 97, 32, 1, 1, 1, 1, 0, 0, 41, 1, 33},
                                                                 {1, 1, 7, 16, 1, 1, 1, 1, 0, 0, 1, 1, 32},
                                                                 {5, 4, 37, 19, 3, 3, 2, 2, 1, 1, 9, 7, 33},
                                                                 {17, 3, 65, 32, 3, 3, 1, 1, 1, 1, 17, 3, 32}}};
            double error = 0;
            unsigned count = 0;
            for (auto p : cases) {
                p[12] = (p[12] & 1u) | 128u;
                for (auto shader :
                     {"conv_dense", "conv_gemm", "conv_gemm_tiled", "conv_pointwise", "conv_pointwise_tiled"}) {
                    if (std::string(shader).find("pointwise") != std::string::npos && (p[4] != 1 || p[5] != 1))
                        continue;
                    error = std::max(error, run_case(ctx, p, shader));
                    ++count;
                }
            }
            for (auto p : cases)
                for (auto shader :
                     {"conv_dense", "conv_gemm", "conv_gemm_tiled", "conv_pointwise", "conv_pointwise_tiled"}) {
                    if (std::string(shader).find("pointwise") != std::string::npos && (p[4] != 1 || p[5] != 1))
                        continue;
                    error = std::max(error, run_case(ctx, p, shader));
                    ++count;
                }
            for (auto p : std::array<std::array<uint32_t, 13>, 3>{{{67, 1, 100, 133, 1, 1, 1, 1, 0, 0, 67, 1, 33},
                                                                   {1, 1, 8, 16, 1, 1, 1, 1, 0, 0, 1, 1, 32},
                                                                   {41, 3, 68, 36, 1, 1, 1, 1, 0, 0, 41, 3, 17}}}) {
                error = std::max(error, run_case(ctx, p, "conv_pointwise_tiled64"));
                ++count;
                if (p[3] % 4 == 0) {
                    error = std::max(error, run_case(ctx, p, "conv_pointwise_vector"));
                    ++count;
                    p[12] = (p[12] & 1u) | 128u;
                    error = std::max(error, run_case(ctx, p, "conv_pointwise_vector"));
                    error = std::max(error, run_case(ctx, p, "conv_pointwise_tiled64"));
                    count += 2;
                }
            }
            for (auto p : std::array<std::array<uint32_t, 13>, 4>{{{1, 1, 192, 768, 1, 1, 1, 1, 0, 0, 1, 1, 0},
                                                                   {3, 1, 97, 68, 1, 1, 1, 1, 0, 0, 3, 1, 1},
                                                                   {4, 1, 64, 64, 1, 1, 1, 1, 0, 0, 4, 1, 33},
                                                                   {1, 1, 128, 192, 1, 1, 1, 1, 0, 0, 1, 1, 129}}}) {
                error = std::max(error, run_case(ctx, p, "conv_pointwise_smallm"));
                ++count;
            }
            std::cout << "{\"device_index\":" << index << ",\"device\":\"" << ctx.properties.deviceName
                      << "\",\"fp32_gelu_epilogues\":true,\"cases\":" << count
                      << ",\"max_absolute_error\":" << std::setprecision(9) << error << "}\n";
            return 0;
        }
        if (depthwise) {
            const std::array<std::array<uint32_t, 13>, 6> cases{{{7, 5, 4, 4, 1, 1, 1, 1, 0, 0, 7, 5, 1},
                                                                 {9, 7, 12, 12, 3, 3, 1, 1, 1, 1, 9, 7, 17},
                                                                 {5, 4, 32, 32, 5, 5, 2, 2, 2, 2, 9, 7, 0},
                                                                 {13, 11, 256, 256, 9, 9, 1, 1, 4, 4, 13, 11, 1},
                                                                 {1, 1, 4, 4, 3, 3, 1, 1, 1, 1, 1, 1, 17},
                                                                 {8, 6, 64, 64, 3, 5, 1, 1, 1, 2, 8, 6, 0}}};
            double error = 0;
            for (auto p : cases)
                error = std::max(error, run_case(ctx, p));
            std::cout << "{\"device_index\":" << index << ",\"device\":\"" << ctx.properties.deviceName
                      << "\",\"fp32_vector_depthwise\":true,\"cases\":" << cases.size()
                      << ",\"max_absolute_error\":" << std::setprecision(9) << error << "}\n";
            return 0;
        }
        const std::array<std::array<uint32_t, 13>, 7> cases{{{41, 1, 97, 133, 1, 1, 1, 1, 0, 0, 41, 1, 1},
                                                             {5, 4, 37, 19, 3, 3, 2, 2, 1, 1, 9, 7, 17},
                                                             {17, 3, 65, 32, 3, 3, 1, 1, 1, 1, 17, 3, 0},
                                                             {16, 2, 128, 256, 1, 1, 1, 1, 0, 0, 16, 2, 17},
                                                             {1, 1, 7, 7, 1, 1, 1, 1, 0, 0, 1, 1, 1},
                                                             {41, 1, 97, 133, 1, 1, 1, 1, 0, 0, 41, 1, 33},
                                                             {5, 4, 37, 19, 3, 3, 2, 2, 1, 1, 9, 7, 32}}};
        double error = 0;
        for (auto p : cases)
            error = std::max(error, run_case(ctx, p));
        std::cout << "{\"device_index\":" << index << ",\"device\":\"" << ctx.properties.deviceName
                  << "\",\"cooperative_16x16x16_fp16_fp32\":true,\"pin_subgroup32\":"
                  << (ctx.required_subgroup_size ? "true" : "false") << ",\"cases\":" << cases.size()
                  << ",\"max_absolute_error\":" << std::setprecision(9) << error << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
