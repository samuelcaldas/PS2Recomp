#include "MiniTest.h"
#include "ps2_runtime.h"
#include <limits>

void register_presentation_frame_tests()
{
    MiniTest::Case("PresentationFrame", [](TestCase &tc) {
        tc.Run("reject invalid input before callback", [](TestCase &t) {
            PS2Runtime rt;
            unsigned calls = 0;
            rt.setFrameCompositor([&](PS2PresentationFrameView) -> std::optional<PS2OwnedPresentationFrame> { ++calls; return std::nullopt; });
            uint8_t pixel[4]{};
            for (auto v : {PS2PresentationFrameView{nullptr,1,1,4}, {pixel,0,1,4}, {pixel,1,0,4}, {pixel,2,1,4}, {pixel,UINT32_MAX,UINT32_MAX,UINT32_MAX}})
                t.IsTrue(!rt.composePresentationFrame(v).has_value(), "invalid geometry rejected");
            t.Equals(calls, 0u, "callback not invoked");
        });
        tc.Run("validate owned output geometry and span", [](TestCase &t) {
            PS2Runtime rt;
            uint8_t pixel[4]{};
            PS2OwnedPresentationFrame out{std::make_shared<const std::vector<uint8_t>>(4),1,1,4};
            rt.setFrameCompositor([&](PS2PresentationFrameView) { return std::optional{out}; });
            for (auto v : {PS2OwnedPresentationFrame{nullptr,1,1,4}, {out.pixels,1921,1,7684}, {out.pixels,1,1081,4}, {out.pixels,2,1,4}, {out.pixels,1,2,4}, {out.pixels,UINT32_MAX,UINT32_MAX,UINT32_MAX}}) {
                out = v;
                t.IsTrue(!rt.composePresentationFrame({pixel,1,1,4}), "invalid output rejected");
            }
        });
        tc.Run("owned lifetime and padded stride", [](TestCase &t) {
            PS2Runtime rt;
            auto owner = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1,2,3,4,99,99,99,99,5,6,7,8});
            rt.setFrameCompositor([owner](PS2PresentationFrameView) { return std::optional{PS2OwnedPresentationFrame{owner,1,2,8}}; });
            uint8_t pixel[4]{};
            auto result = rt.composePresentationFrame({pixel,1,1,4});
            owner.reset();
            rt.setFrameCompositor({});
            t.IsTrue(result && (*result->pixels)[8] == 5, "result retains ownership");
            PS2PresentationFrameCache cache;
            cache.update({pixel,1,1,4}, [&](auto) { return result; });
            t.Equals(cache.pixels.size(), size_t(8), "padding stripped");
            t.Equals(unsigned(cache.pixels[4]), 5u, "second row uses stride");
        });
        tc.Run("sampling follows retained mode at identical extent", [](TestCase &t) {
            PS2PresentationFrameCache cache;
            uint8_t pixel[4]{1,2,3,4};
            bool compose = true;
            unsigned calls = 0;
            auto compositor = [&](auto) -> std::optional<PS2OwnedPresentationFrame> {
                ++calls;
                if (!compose) return std::nullopt;
                return PS2OwnedPresentationFrame{std::make_shared<const std::vector<uint8_t>>(4,42),1,1,4};
            };
            t.IsTrue(!cache.composed && !cache.usePointSampling(), "legacy defaults to bilinear");
            t.IsTrue(cache.update({pixel,1,1,4}, compositor), "composed frame accepted");
            t.IsTrue(cache.composed && cache.usePointSampling() && cache.pixels[0] == 42, "composed frame selects point");
            compose = false;
            t.IsTrue(!cache.update({nullptr,0,0,0}, compositor), "missing source does not update");
            t.IsTrue(calls == 1 && cache.composed && cache.usePointSampling() &&
                     cache.width == 1 && cache.height == 1 && cache.pixels.size() == 4 && cache.pixels[0] == 42,
                     "missing source retains pixels extent and composed sampling");
            t.IsTrue(cache.update({pixel,1,1,4}, compositor), "legacy frame accepted at same extent");
            t.IsTrue(!cache.composed && !cache.usePointSampling() && cache.width == 1 && cache.height == 1 &&
                     cache.pixels[0] == 1, "same extent switches back to legacy bilinear");
            t.IsTrue(!cache.update({nullptr,0,0,0}, compositor) && !cache.composed &&
                     !cache.usePointSampling() && cache.pixels[0] == 1 && calls == 2,
                     "missing source retains legacy mode too");
        });
        tc.Run("shared prepare composes before upload and retains coherent resize", [](TestCase &t) {
            PS2Runtime rt;
            PS2PresentationFrameCache cache;
            uint8_t pixel[4]{1,2,3,4};
            t.IsTrue(!rt.composePresentationFrame({pixel,1,1,4}), "no compositor preserves legacy");
            bool composed = false;
            rt.setFrameCompositor([&](auto) {
                composed = true;
                return std::optional{PS2OwnedPresentationFrame{std::make_shared<const std::vector<uint8_t>>(24,42),3,2,12}};
            });
            cache.update({pixel,1,1,4}, [&](auto view) { return rt.composePresentationFrame(view); });
            t.IsTrue(composed && cache.width == 3 && cache.height == 2 && cache.pixels.size() == 24 && cache.pixels[0] == 42, "upload source already composed");
            cache.update({nullptr,0,0,0}, [&](auto view) { return rt.composePresentationFrame(view); });
            t.IsTrue(cache.width == 3 && cache.height == 2 && cache.pixels.size() == 24, "missing frame retains data and extent");
            rt.setFrameCompositor([](auto) -> std::optional<PS2OwnedPresentationFrame> { return std::nullopt; });
            cache.update({pixel,1,1,4}, [&](auto view) { return rt.composePresentationFrame(view); });
            t.IsTrue(cache.width == 1 && cache.height == 1 && cache.pixels[0] == 1, "nullopt returns to legacy geometry");
        });
    });
}
