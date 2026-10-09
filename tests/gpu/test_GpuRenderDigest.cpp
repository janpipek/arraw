#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "support/RenderDigest.h"

#include <Develop.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace arraw;
using namespace arraw::test;

/// Hidden: writes the GPU render digest when ARRAW_DIGEST_OUT names a file (see RenderDigest.h).
TEST_CASE("The GPU render digest", "[.digest]") {
    const std::string path = digestOutputPath();
    if (path.empty()) {
        SUCCEED("ARRAW_DIGEST_OUT is not set");
        return;
    }
    GpuContext& context = gpuContext();
    DigestWriter writer(path);
    const auto sources = digestSources();
    const auto states = digestStates();
    const auto requests = digestRequests();
    for (const auto& source : sources) {
        for (const auto& state : states) {
            for (const auto& request : requests) {
                const std::string base =
                    "gpu/" + source.name + "/" + state.name + "/" + request.name;
                for (const Stage stop : digestStages) {
                    writer.render(base + "/until-" + digestStageName(stop), [&] {
                        return developOnGpu(context, source.buffer, state.state, stop,
                                            request.request)
                            .readBack();
                    });
                }
                writer.render(base + "/sample", [&] {
                    return sampleOnGpu(context, source.buffer, state.state, Tap::CurveInput,
                                       request.request);
                });
            }
        }
    }
}
