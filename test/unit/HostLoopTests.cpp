#include "../../examples/host-loop/BoingHost.hpp"
#include "../../examples/host-loop/HostLoopCapture.hpp"
#include "Bdd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace webfront::bdd;

namespace {
using namespace boing;

struct Rgb {
    int r, g, b;
};
Rgb rgbOf(std::uint32_t value) {
    return {static_cast<int>(value & 0xffU), static_cast<int>((value >> 8) & 0xffU), static_cast<int>((value >> 16) & 0xffU)};
}

// The pixel in which the camera of BoingScene.hpp sees `point`.
std::pair<int, int> pixelSeeing(const Scene& scene, Vec point) {
    const Vec   toPoint = point - Vec{0.F, 2.3F, 8.5F};
    const float aspect  = static_cast<float>(scene.width) / static_cast<float>(scene.height);
    const float u       = (toPoint.x / -toPoint.z / (aspect * 0.42F) + 1.F) / 2.F;
    const float v       = (1.F - (toPoint.y / -toPoint.z + 0.12F) / 0.42F) / 2.F;
    return {static_cast<int>(u * static_cast<float>(scene.width)), static_cast<int>(v * static_cast<float>(scene.height))};
}

std::vector<std::uint32_t> cpuImage(const Scene& scene, unsigned threads) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height));
    renderCpu(scene, pixels.data(), threads);
    return pixels;
}

std::vector<std::uint32_t> unpacked(const std::vector<std::uint32_t>& runs) {
    std::vector<std::uint32_t> pixels;
    for (std::size_t i = 0; i + 1 < runs.size(); i += 2)
        pixels.insert(pixels.end(), runs[i], runs[i + 1]);
    return pixels;
}

// A consumer that counts the images it receives and keeps the last one.
struct Consumer {
    int   received{0};
    Frame last;
    Host::Deliver deliver() {
        return [this](const Frame& frame) {
            ++received;
            last = frame;
        };
    }
};
}  // namespace

const Scenario hostLoopCapture{"Host loop: capture refuses unsupported modes and reports failures", [] {
    using webfront::frontend::CaptureResult;
    then("capture needs a native window, including when benchmark would otherwise take precedence", [] {
        validateCapture(true, true, false, false);
        validateCapture(false, false, true, true);
        requireThrows<std::invalid_argument>([] { validateCapture(true, false, false, false); });
        requireThrows<std::invalid_argument>([] { validateCapture(true, true, true, false); });
        requireThrows<std::invalid_argument>([] { validateCapture(true, true, false, true); });
    });
    then("an unavailable or timed out capture preserves its diagnostic", [] {
        requireThrows<std::runtime_error>([] {
            saveCapture(CaptureResult{std::unexpected("window did not answer")}, "unused.png");
        }, contains("window did not answer"));
    });
    then("an unwritable destination fails", [] {
        // Opening an existing directory as a file fails on every supported platform.
        requireThrows<std::runtime_error>([] {
            saveCapture(CaptureResult{std::vector<std::uint8_t>{1, 2, 3}}, ".");
        }, contains("cannot write"));
    });
    then("a successful capture writes every byte", [] {
        const auto directory = std::filesystem::temp_directory_path()
            / ("webfront-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory));
        struct Cleanup {
            std::filesystem::path directory;
            ~Cleanup() { std::filesystem::remove_all(directory); }
        } cleanup{directory};
        const auto path = directory / "capture.png";
        saveCapture(CaptureResult{std::vector<std::uint8_t>{1, 2, 3}}, path.string());
        std::ifstream file{path, std::ios::binary};
        std::array<char, 3> bytes{};
        file.read(bytes.data(), 3);
        require(file.gcount() == 3);
        require(bytes == std::array<char, 3>{1, 2, 3});
        require(file.peek() == std::char_traits<char>::eof());
    });
#ifdef __linux__
    then("a buffered write failure also fails when the file is closed", [] {
        requireThrows<std::runtime_error>([] {
            saveCapture(CaptureResult{std::vector<std::uint8_t>{1, 2, 3}}, "/dev/full");
        }, contains("cannot write"));
    });
#endif
}};

const Scenario hostLoopIntersections{"Host loop: rays meet the ball and the floor where geometry says", [] {
    const Scene scene = sceneAt(0.F, false, 320, 240, 1);
    then("the ball starts on the floor, at one side", [&] {
        require(scene.ball.x == 2.6F);
        require(scene.ball.y == 1.F);
        require(scene.ball.z == 0.F);
    });
    then("a ray aimed at its centre stops one radius before it, and a ray aimed away misses", [&] {
        const Vec origin = scene.ball + Vec{0.F, 0.F, 5.F};
        require(std::fabs(hitBall(scene, origin, {0.F, 0.F, -1.F}) - 4.F) < 1e-4F);
        require(hitBall(scene, origin, {0.F, 0.F, 1.F}) == 0.F);
        require(hitBall(scene, origin, normalize({1.F, 0.F, -1.F})) == 0.F);
    });
    then("a tangent ray grazes it and a ray leaving its surface does not meet it again", [&] {
        require(hitBall(scene, scene.ball + Vec{1.0001F, 0.F, 5.F}, {0.F, 0.F, -1.F}) == 0.F);
        require(hitBall(scene, scene.ball + Vec{0.999F, 0.F, 5.F}, {0.F, 0.F, -1.F}) > 4.F);
        require(hitBall(scene, scene.ball + Vec{0.F, 1.F, 0.F}, {0.F, 1.F, 0.F}) == 0.F);
    });
    then("only a descending ray meets the floor, at the height of its origin over its slope", [&] {
        require(std::fabs(hitFloor({0.F, 3.F, 0.F}, normalize({0.F, -1.F, -1.F})) - 3.F * std::sqrt(2.F)) < 1e-4F);
        require(hitFloor({0.F, 3.F, 0.F}, {0.F, 0.F, -1.F}) == 0.F);
        require(hitFloor({0.F, 3.F, 0.F}, {0.F, 1.F, 0.F}) == 0.F);
    });
}};

const Scenario hostLoopAnimation{"Host loop: the animation is a function of time", [] {
    then("the ball stays above the floor and between the sides, and bounces with a period of 1.2 s", [&] {
        for (int tick = 0; tick < 400; ++tick) {
            const Scene scene = sceneAt(static_cast<float>(tick) * 0.05F, false, 320, 240, 1);
            require(scene.ball.y >= 1.F);
            require(scene.ball.y <= 3.1001F);
            require(std::fabs(scene.ball.x) <= 2.6001F);
            require(std::fabs(scene.spinCos * scene.spinCos + scene.spinSin * scene.spinSin - 1.F) < 1e-4F);
        }
        require(std::fabs(sceneAt(0.6F, false, 320, 240, 1).ball.y - 3.1F) < 1e-3F);
        require(std::fabs(sceneAt(1.2F, false, 320, 240, 1).ball.y - 1.F) < 1e-3F);
    });
    then("the same time gives the same scene, and another time another one", [&] {
        const Scene a = sceneAt(2.5F, true, 640, 480, 2), b = sceneAt(2.5F, true, 640, 480, 2), c = sceneAt(2.6F, true, 640, 480, 2);
        require(a.ball.x == b.ball.x);
        require(a.ball.y == b.ball.y);
        require(a.light.x == b.light.x);
        require(a.ball.x != c.ball.x);
        require(a.glass == 1);
    });
}};

const Scenario hostLoopCpuImage{"Host loop: the CPU image is deterministic and shows the scene", [] {
    const Scene scene  = sceneAt(0.1F, false, 320, 240, 1);  // the ball is low: its reflection is in the image
    const auto  image  = cpuImage(scene, 1);
    const auto  at     = [&](std::pair<int, int> where) { return rgbOf(image[static_cast<std::size_t>(where.second * scene.width + where.first)]); };
    then("any number of threads computes the same pixels", [&] {
        require(cpuImage(scene, 4) == image);
        require(cpuImage(scene, 7) == image);
    });
    then("every pixel is opaque", [&] {
        for (const auto value : image)
            require((value >> 24) == 0xffU);
    });
    then("the top of the image is sky, bluer than it is red", [&] {
        const auto top = at({160, 2});
        require(top.b > top.r + 20);
    });
    then("the ball is a red and white checker", [&] {
        int red = 0, white = 0;
        for (int dy = -20; dy <= 20; dy += 4)
            for (int dx = -20; dx <= 20; dx += 4) {
                auto [x, y]      = pixelSeeing(scene, scene.ball);
                const auto color = at({x + dx, y + dy});
                red   += color.r > 120 && color.g < 80 && color.b < 80;
                white += color.r > 120 && color.g > 120 && color.b > 120;
            }
        require(red > 20);
        require(white > 20);
    });
    then("the floor is darker in the shadow of the ball than beside it", [&] {
        // The point of the floor on the line from the light through the centre of the ball, and one well outside the shadow.
        const float scale = scene.light.y / (scene.light.y - scene.ball.y);
        const Vec   shadow{scene.light.x + (scene.ball.x - scene.light.x) * scale, 0.F, scene.light.z + (scene.ball.z - scene.light.z) * scale};
        const Vec   lit{shadow.x + 2.5F, 0.F, shadow.z};
        // Half a square away from the grid lines.
        const auto between = [](float value) { return std::floor(value) + 0.5F; };
        const auto dark    = at(pixelSeeing(scene, {between(shadow.x), 0.F, between(shadow.z)}));
        const auto bright  = at(pixelSeeing(scene, {between(lit.x), 0.F, between(lit.z)}));
        require(dark.g + 20 < bright.g);
    });
    then("the floor reflects the ball: without the ball the floor under it is another colour", [&] {
        Scene away  = scene;
        away.ball.x += 100.F;
        away.light  = scene.light;
        const auto empty = cpuImage(away, 1);
        // The mirror image of the ball's centre is seen through the floor, one ball height below it.
        auto [x, y] = pixelSeeing(scene, {scene.ball.x, -scene.ball.y, scene.ball.z});
        int differing = 0;
        for (int dy = -6; dy <= 6; ++dy)
            for (int dx = -6; dx <= 6; ++dx) {
                const auto index = static_cast<std::size_t>((y + dy) * scene.width + x + dx);
                differing        += image[index] != empty[index];
            }
        require(differing > 80);
    });
    then("glass changes the ball, dims the day and sends coloured beams through the scene", [&] {
        Scene glassy = scene;
        glassy.glass = 1;
        const auto glass = cpuImage(glassy, 2);
        require(cpuImage(glassy, 5) == glass);
        auto [x, y]     = pixelSeeing(scene, scene.ball);
        int   differing = 0;
        for (int dy = -20; dy <= 20; dy += 2)
            for (int dx = -20; dx <= 20; dx += 2)
                differing += glass[static_cast<std::size_t>((y + dy) * scene.width + x + dx)] != image[static_cast<std::size_t>((y + dy) * scene.width + x + dx)];
        require(differing > 300);
        // Nothing is green in the opaque image, and its sky is blue-grey. The beams tint the top of the image red in
        // places, and the image green in others.
        const auto tints = [&](const std::vector<std::uint32_t>& pixels) {
            int red = 0, green = 0;
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                const auto color = rgbOf(pixels[i]);
                red   += i < 60U * 320U && color.r > color.b + 10;
                green += color.g > color.b + 10 && color.g > color.r + 10;
            }
            return std::pair{red, green};
        };
        require(tints(image) == std::pair{0, 0});
        const auto note = info("red ", tints(glass).first, " green ", tints(glass).second);
        require(tints(glass).first > 200);
        require(tints(glass).second > 200);
    });
    then("the flat colours of the opaque image pack into few runs, the beams of the glass one into more runs than pixels", [&] {
        Scene glassy = scene;
        glassy.glass = 1;
        std::vector<std::uint32_t> runs;
        pack(image, runs);
        require(runs.size() * 2 < image.size());
        require(unpacked(runs) == image);
        const auto glass = cpuImage(glassy, 2);
        pack(glass, runs);
        require(runs.size() > glass.size());
        require(unpacked(runs) == glass);
    });
    then("four rays per pixel smooth the edge of the ball", [&] {
        Scene fine   = scene;
        fine.samples = 2;
        const auto smooth = cpuImage(fine, 2);
        require(smooth != image);
        require(smooth[2 * 320 + 160] == image[2 * 320 + 160]);
    });
}};

const Scenario hostLoopPack{"Host loop: runs of equal pixels pack into their length and the pixel", [] {
    std::vector<std::uint32_t> runs{7U};
    pack({}, runs);
    require(runs.empty());
    pack({5U}, runs);
    require(runs == std::vector<std::uint32_t>{1U, 5U});
    pack({5U, 5U, 5U, 9U, 5U, 5U}, runs);
    require(runs == std::vector<std::uint32_t>{3U, 5U, 1U, 9U, 2U, 5U});
    require(unpacked(runs) == std::vector<std::uint32_t>{5U, 5U, 5U, 9U, 5U, 5U});
}};

const Scenario hostLoopSettings{"Host loop: settings are validated before they reach the loop", [] {
    Host     host{{}, 2};
    Consumer consumer;
    then("the image has the size and rays of the chosen quality", [&] {
        for (std::uint32_t quality = 0; quality < qualities.size(); ++quality) {
            host.set({Engine::cpu, false, false, quality});
            host.request(1, consumer.deliver());
            require(host.iterate(0.));
            require(consumer.last.scene.width == qualities[quality].width);
            require(consumer.last.scene.height == qualities[quality].height);
            require(consumer.last.scene.samples == qualities[quality].samples);
            require(consumer.last.pixels.size() == static_cast<std::size_t>(qualities[quality].width * qualities[quality].height));
            require(consumer.last.settings.quality == quality);
            require(consumer.last.computeMs > 0.);
            require(consumer.last.copyMs == 0.);
        }
    });
    then("each change has a revision, which the images computed afterwards carry", [&] {
        const auto first = host.set({Engine::cpu, false, false, 0});
        require(host.set({Engine::cpu, true, false, 0}) == first + 1);
        requireThrows<std::out_of_range>([&] { host.set({Engine::cpu, false, false, 99}); });
        host.request(1, consumer.deliver());
        require(host.iterate(0.));
        require(consumer.last.revision == first + 1);
    });
    then("the opaque image comes with its runs, the glass one without", [&] {
        host.set({Engine::cpu, true, false, 0});
        host.request(1, consumer.deliver());
        require(host.iterate(0.));
        require(unpacked(consumer.last.packed) == consumer.last.pixels);
        require(consumer.last.packed.size() * 2 < consumer.last.pixels.size());
        host.set({Engine::cpu, true, true, 0});
        host.request(1, consumer.deliver());
        require(host.iterate(0.));
        require(consumer.last.packed.empty());
        require(consumer.last.pixels.size() == 320U * 240U);
    });
    then("an unknown quality or engine is refused and changes nothing", [&] {
        host.set({Engine::cpu, false, true, 0});
        requireThrows<std::out_of_range>([&] { host.set({Engine::cpu, false, false, static_cast<std::uint32_t>(qualities.size())}); });
        requireThrows<std::out_of_range>([&] { host.set({static_cast<Engine>(2), false, false, 0}); });
        host.request(1, consumer.deliver());
        require(host.iterate(0.));
        require(consumer.last.settings.glass);
        require(consumer.last.settings.quality == 0U);
    });
    then("CUDA is accepted where a device exists and refused, with its reason, where none does", [&] {
        if (host.device().empty()) {
            requireThrows<std::runtime_error>([&] { host.set({Engine::cuda, false, false, 0}); }, contains("CUDA is unavailable"));
            requireThrows<std::runtime_error>([] { Host refused{{Engine::cuda, false, false, 0}}; }, contains("CUDA is unavailable"));
            host.request(1, consumer.deliver());
            require(host.iterate(0.));
            require(consumer.last.settings.engine == Engine::cpu);
        } else {
            host.set({Engine::cuda, false, false, 0});
            host.request(1, consumer.deliver());
            require(host.iterate(0.));
            require(consumer.last.settings.engine == Engine::cuda);
        }
    });
}};

const Scenario hostLoopPublication{"Host loop: a consumer receives one image per request, and none queues", [] {
    Host     host{{Engine::cpu, false, false, 0}, 2};
    Consumer first, second;
    then("nothing is computed while no consumer asks, yet the scene goes on", [&] {
        require(!host.iterate(0.5));
        require(!host.iterate(0.5));
        require(host.sceneSeconds() == 1.);
        require(first.received == 0);
    });
    then("a request is answered by exactly one image", [&] {
        host.request(1, first.deliver());
        require(host.iterate(0.1));
        require(first.received == 1);
        require(!host.iterate(0.1));
        require(!host.iterate(0.1));
        require(first.received == 1);
    });
    then("a consumer that asks twice before the loop turns still receives one image", [&] {
        const int before = first.received;
        host.request(1, first.deliver());
        host.request(1, first.deliver());
        require(host.iterate(0.1));
        require(!host.iterate(0.1));
        require(first.received == before + 1);
    });
    then("a duplicate after the batch was taken still receives only its original image", [&] {
        // Consumer 1 is delivered first: consumer 2 has left waiting but has not begun receiving its image.
        host.request(1, [&](const Frame& frame) {
            first.deliver()(frame);
            host.request(2, second.deliver());
        });
        host.request(2, second.deliver());
        require(host.iterate(0.1));
        require(first.received == 1);
        require(second.received == 1);
        require(!host.iterate(0.1));
        require(second.received == 1);
    });
    then("a consumer can request its next image as soon as its delivery begins", [&] {
        host.request(1, [&](const Frame& frame) {
            first.deliver()(frame);
            host.request(1, first.deliver());
        });
        require(host.iterate(0.1));
        require(host.iterate(0.1));
        require(first.received == 2);
        require(!host.iterate(0.1));
    });
    then("a failed delivery releases the remaining in-flight consumers", [&] {
        host.request(1, [](const Frame&) { throw std::runtime_error("delivery failed"); });
        host.request(2, second.deliver());
        requireThrows<std::runtime_error>([&] { host.iterate(0.1); });
        host.request(2, second.deliver());
        require(host.iterate(0.1));
        require(second.received == 1);
    });
    then("two consumers receive the same image, and a slow one misses images instead of queuing them", [&] {
        first = {};
        host.request(1, first.deliver());
        host.request(2, second.deliver());
        require(host.iterate(0.1));
        require(first.last.sequence == second.last.sequence);
        require(first.last.pixels == second.last.pixels);
        // The first consumer asks for three more images; the second, slower, asks once at the end.
        for (int i = 0; i < 3; ++i) {
            host.request(1, first.deliver());
            require(host.iterate(0.1));
        }
        host.request(2, second.deliver());
        require(host.iterate(0.1));
        require(first.received == 4);
        require(second.received == 2);
        require(second.last.sequence == first.last.sequence + 1);
    });
    then("pause freezes the scene, not the answers", [&] {
        host.set({Engine::cpu, true, false, 0});
        host.request(1, first.deliver());
        require(host.iterate(1.));
        const Frame still = first.last;
        const double time = host.sceneSeconds();
        host.request(1, first.deliver());
        require(host.iterate(1.));
        require(host.sceneSeconds() == time);
        require(first.last.pixels == still.pixels);
        require(first.last.sequence == still.sequence + 1);
        require(first.last.settings.paused);
        host.set({Engine::cpu, false, false, 0});
        host.request(1, first.deliver());
        require(host.iterate(0.3));
        require(first.last.pixels != still.pixels);
    });
}};

const Scenario hostLoopRun{"Host loop: the loop answers from its own thread and releases its consumers when it ends", [] {
    Host              host{{Engine::cpu, false, false, 0}, 2};
    std::atomic<int>  received{0};
    std::thread::id   deliveredOn;
    const auto        held = std::make_shared<int>(0);
    std::jthread      loop([&](std::stop_token stopping) { host.run(stopping); });
    then("a request from another thread is answered on the loop's thread", [&] {
        host.request(1, [&, held](const Frame&) {
            deliveredOn = std::this_thread::get_id();
            ++received;
        });
        requireEventually([&] { return received == 1; });
        require(deliveredOn == loop.get_id());
        require(deliveredOn != std::this_thread::get_id());
    });
    then("stop ends the loop, and a request that arrives afterwards is dropped with what it holds", [&] {
        host.stop();
        loop.join();
        host.request(1, [&, held](const Frame&) { ++received; });
        require(!host.iterate(0.));
        require(held.use_count() == 1);
        require(received <= 1);
    });
}};

const Scenario hostLoopStopToken{"Host loop: the application's stop token ends the loop and drops a pending request", [] {
    Host         host{{Engine::cpu, true, false, 0}, 1};
    const auto   held = std::make_shared<int>(0);
    std::jthread loop([&](std::stop_token stopping) { host.run(stopping); });
    loop.request_stop();
    loop.join();
    host.request(1, [held](const Frame&) {});
    require(held.use_count() == 1);
}};

const Scenario hostLoopCuda{"Host loop: CUDA computes the image of the CPU, within the rounding of the device", [] {
    Host host{{}, 4};
    if (host.device().empty())
        skip("no CUDA renderer in this build, or no CUDA device on this machine");
    for (const auto quality : qualities)
        for (const bool glass : {false, true})
            for (const float seconds : {0.F, 0.4F, 3.3F, 9.7F}) {
                const auto note = info(quality.width, "x", quality.height, " samples ", quality.samples, " glass ", glass, " at ", seconds, " s");
                Frame      cpu, cuda;
                cpu.scene = cuda.scene = sceneAt(seconds, glass, quality.width, quality.height, quality.samples);
                cuda.settings.engine   = Engine::cuda;
                render(cpu, host.threads());
                render(cuda, host.threads());
                require(cuda.pixels.size() == cpu.pixels.size());
                // The device contracts multiplications and additions and has its own acos and atan2: a channel may
                // round the other way, and a pixel on the edge of a square or of a grid line may fall on the other side.
                std::size_t rounded = 0, different = 0;
                for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
                    const auto a = rgbOf(cpu.pixels[i]), b = rgbOf(cuda.pixels[i]);
                    const int  gap = std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
                    rounded   += gap > 0;
                    different += gap > 2;
                }
                require(different * 1000 <= cpu.pixels.size());  // at most 0.1 % of the pixels
                require(rounded * 10 <= cpu.pixels.size());      // at most 10 % differ at all
                require(cuda.computeMs > 0.);
                require(cuda.copyMs > 0.);
            }
}};
