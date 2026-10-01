#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
extern "C" {
#include "lw-ppocr-c/ppocr/det_internal.h"
#include "lw-ppocr-c/ppocr/crop_internal.h"
#include "lw-ppocr-c/simd/simd_kernels.h"
}
static void require(bool value) {
    if (!value)
        throw std::runtime_error("geometry unit assertion");
}
int main() {
    uint32_t w = 0, h = 0;
    float wr = 0, hr = 0;
    require(lw_det_compute_size(2000, 1000, 960, &w, &h, &wr, &hr) == 0 && w == 960 && h == 480);
    require(lw_det_compute_size(100, 50, 960, &w, &h, &wr, &hr) == 0 && w == 96 && h == 64);
    require(lw_det_compute_size(0, 50, 960, &w, &h, &wr, &hr) == 1);
    std::vector<float> map(32 * 32, 0);
    lw_detection_box boxes[10]{};
    uint32_t count = 0;
    auto db = [&] {
        return lw_db_postprocess_f32(map.data(), 32, 32, .3f, .6f, 1.5f, 0, 10, 32, 32, 1, 1, boxes, 10, &count);
    };
    require(db() == 0 && count == 0);
    for (int y = 10; y < 20; ++y)
        for (int x = 5; x < 27; ++x)
            map[y * 32 + x] = .9f;
    require(db() == 0 && count == 1 && std::abs(boxes[0].score - .9f) < 1e-5f);
    require(boxes[0].x1 >= 0 && boxes[0].y1 >= 0 && boxes[0].x3 <= 32 && boxes[0].y3 <= 32);
    require(lw_db_postprocess_f32(map.data(), 32, 32, .3f, .6f, 1.5f, 0, 10, 32, 32, 1, 1, boxes, 0, &count) == 6 &&
            count == 1);
    map[0] = std::numeric_limits<float>::quiet_NaN();
    require(db() == 1);
    uint8_t bitmap[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1}, interior[9]{};
    lw_avx2_interior_bitmap_u8(bitmap, interior, 3, 3);
    require(interior[4] == 1 && interior[0] == 0);
    bitmap[1] = 0;
    lw_avx2_interior_bitmap_u8(bitmap, interior, 3, 3);
    require(interior[4] == 0);
    lw_detection_box box{1, 1, 5, 1, 5, 3, 1, 3, 1, 0};
    uint64_t bytes = 0;
    require(lw_crop_quad_size(&box, &w, &h, &bytes) == 0 && w == 4 && h == 2 && bytes == 24);
    std::vector<uint8_t> source(10 * 10 * 3), crop(24);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            for (int c = 0; c < 3; ++c)
                source[(y * 10 + x) * 3 + c] = static_cast<uint8_t>(y * 10 + x);
    require(lw_crop_quad_bgr_u8(source.data(), 300, 10, 10, 30, &box, crop.data(), crop.size(), &w, &h, &bytes) == 0);
    require(crop[0] == 11 && crop[9] == 14 && crop[12] == 21);
    require(lw_crop_quad_bgr_u8(source.data(), 299, 10, 10, 30, &box, crop.data(), crop.size(), &w, &h, &bytes) == 9);
    lw_rotate_bgr_u8_180(crop.data(), 4, 2);
    require(crop[0] == 24 && crop[21] == 11);
    box = {1, 1, 3, 1, 3, 5, 1, 5, 1, 0};
    require(lw_crop_quad_size(&box, &w, &h, &bytes) == 0 && w == 4 && h == 2);
    lw_detection_box sorted[2] = {{20, 20, 21, 20, 21, 21, 20, 21, 1, 0}, {2, 20, 3, 20, 3, 21, 2, 21, 1, 0}};
    require(lw_sort_detection_boxes(sorted, 2, 0) == 0 && sorted[0].x1 == 2);
    require(lw_sort_detection_boxes(sorted, 2, 99) == 1);
}
