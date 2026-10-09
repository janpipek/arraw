#include "support/RenderDigest.h"

#include <Develop.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// Hidden: writes the CPU render digest when ARRAW_DIGEST_OUT names a file (see RenderDigest.h).
TEST_CASE("The CPU render digest", "[.digest]") {
    const std::string path = digestOutputPath();
    if (path.empty()) {
        SUCCEED("ARRAW_DIGEST_OUT is not set");
        return;
    }
    DigestWriter writer(path);
    const auto sources = digestSources();
    const auto requests = digestRequests();
    const auto run = [&](const std::vector<DigestState>& states) {
        for (const auto& source : sources) {
            for (const auto& state : states) {
                for (const auto& request : requests) {
                    const std::string base =
                        "cpu/" + source.name + "/" + state.name + "/" + request.name;
                    writer.render(base + "/develop", [&] {
                        return develop(source.buffer, state.state, request.request);
                    });
                    for (const Stage stop : digestStages) {
                        writer.render(base + "/until-" + digestStageName(stop), [&] {
                            return developUntil(source.buffer, state.state, stop, request.request)
                                .readBack();
                        });
                    }
                    writer.render(base + "/sample", [&] {
                        return sample(source.buffer, state.state, Tap::CurveInput, request.request);
                    });
                }
            }
        }
    };
    run(digestStates());
    // Appended after the matrix above, whose lines stay as they were.
    run(digestMaskStates());
}
