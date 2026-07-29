#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "ppocr_db_postprocess.hpp"

TEST_CASE("DBPostProcess empty probability map returns 0 boxes", "[paddle][db]") {
  std::vector<float> prob_map(100 * 100, 0.0f);
  sublift::paddle::DBPostProcessOptions opts;

  auto res = sublift::paddle::db_postprocess(prob_map.data(), 100, 100, 100, 100, opts);
  CHECK(res.quads.empty());
  CHECK(res.aabbs.empty());
}

TEST_CASE("DBPostProcess synthetic high prob rectangle detection and unclip", "[paddle][db]") {
  int w = 100;
  int h = 100;
  std::vector<float> prob_map(w * h, 0.0f);

  // Fill a 40x20 rectangle with prob 0.95
  for (int y = 30; y < 50; ++y) {
    for (int x = 20; x < 60; ++x) {
      prob_map[y * w + x] = 0.95f;
    }
  }

  sublift::paddle::DBPostProcessOptions opts;
  opts.det_thresh = 0.3f;
  opts.det_box_thresh = 0.6f;
  opts.unclip_ratio = 1.6f;

  auto res = sublift::paddle::db_postprocess(prob_map.data(), h, w, h, w, opts);
  REQUIRE(res.aabbs.size() >= 1);
  CHECK(res.quads[0].score >= 0.8f);

  // AABB bounding box should cover original 20..60 x 30..50 plus unclip padding
  CHECK(res.aabbs[0].width >= 20);
  CHECK(res.aabbs[0].height >= 5);
}
