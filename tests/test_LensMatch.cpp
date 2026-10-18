#include "LensFactsReader.h"
#include "support/Fixtures.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"

#include <ImageImport.h>
#include <LensMatch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Approx;

namespace {

/// @brief Facts of a shot on the reference body, for a lens named by @p names.
LensFacts on80D(std::vector<std::string> names) {
    LensFacts facts;
    facts.cameraMaker = "Canon";
    facts.cameraModel = "Canon EOS 80D";
    facts.lensNames = std::move(names);
    return facts;
}

LensMatch matched(const LensMatchResult& result) {
    REQUIRE(std::holds_alternative<LensMatch>(result));
    return std::get<LensMatch>(result);
}

LensNoMatch refused(const LensMatchResult& result, LensProblem problem) {
    REQUIRE(std::holds_alternative<LensNoMatch>(result));
    const auto& none = std::get<LensNoMatch>(result);
    REQUIRE(none.problem == problem);
    return none;
}

const LensIdentity kitLens{"Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM"};

} // namespace

TEST_CASE("Canon's kit lens is found on the 80D by the name its EXIF gives", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"EF-S18-135mm f/3.5-5.6 IS USM"});
    facts.focalLength = 50.0;
    facts.aperture = 5.6;

    const LensMatch match = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(match.lens == kitLens);
    CHECK(match.name == "EF-S18-135mm f/3.5-5.6 IS USM");
    CHECK(match.exactName);
    CHECK(match.cameraListed);
    CHECK(match.cropFactor == Approx(1.6));
    CHECK(match.cropSource == CropSource::Camera);
    CHECK(match.focalLength == 50.0);
    CHECK(match.focalLengthRead);
}

TEST_CASE("An EF lens is found on an EF-S body", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"EF50mm f/1.8 STM"});
    facts.focalLength = 50.0;

    const LensMatch match = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(match.lens == LensIdentity{"Canon", "Canon EF 50mm f/1.8 STM"});
    // The prime's profile was measured on full frame; the image's crop is the camera's.
    CHECK(match.cropFactor == Approx(1.6));
}

TEST_CASE("An EF body takes an EF lens and not an EF-S one", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts;
    facts.cameraMaker = "Canon";
    facts.cameraModel = "Canon EOS 5D Mark IV";
    facts.focalLength = 50.0;

    facts.lensNames = {"EF50mm f/1.8 STM"};
    const LensMatch prime = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(prime.cropFactor == Approx(1.0));

    facts.lensNames = {"EF-S18-135mm f/3.5-5.6 IS USM"};
    refused(matchLens(facts, test::fixtureDatabase()), LensProblem::LensNotInDatabase);
}

TEST_CASE("Names are tried in order, and the first that finds a lens decides", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D(
        {"Sigma 99mm f/0.5 Imaginary", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", "EF50mm f/1.8 STM"});
    facts.focalLength = 50.0;

    const LensMatch match = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(match.lens == kitLens);
    CHECK(match.name == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
}

TEST_CASE("The maker-note names stand in when the EXIF names no lens", "[lens]") {
    std::vector<std::string> names;
    lensfacts::addName(names, "  ");
    lensfacts::addName(names, "----");
    lensfacts::addName(names, "EF-S18-135mm f/3.5-5.6 IS USM");
    // The same name spelled apart is the same name.
    lensfacts::addName(names, "ef-s 18-135mm f/3.5-5.6 is usm");
    lensfacts::addName(names, "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    CHECK(names == std::vector<std::string>{"EF-S18-135mm f/3.5-5.6 IS USM",
                                            "Canon EF-S 18-135mm f/3.5-5.6 IS USM"});

    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    // With the EXIF's LensModel missing, the first name is the maker note's.
    LensFacts facts = on80D({"Canon EF-S 18-135mm f/3.5-5.6 IS USM"});
    facts.focalLength = 24.0;
    CHECK(matched(matchLens(facts, test::fixtureDatabase())).lens == kitLens);
}

TEST_CASE("A third-party lens is found by the name its EXIF gives", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"Sigma 18-35mm F1.8 DC HSM"});
    facts.minFocal = 18.0;
    facts.maxFocal = 35.0;
    facts.maxAperture = 1.8;
    facts.focalLength = 24.0;
    facts.aperture = 2.8;

    const LensMatch match = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(match.lens == LensIdentity{"Sigma", "Sigma 18-35mm f/1.8 DC HSM [A]"});
    CHECK_FALSE(match.exactName);
}

TEST_CASE("A name from a lens ID that belongs to another focal range is filtered out",
          "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    // A Sigma 18-35 reports a Canon ID whose name is the Canon 50: right by name, wrong by
    // everything else the file says.
    LensFacts facts = on80D({"Canon EF 50mm f/1.8 STM"});
    facts.minFocal = 18.0;
    facts.maxFocal = 35.0;
    facts.maxAperture = 1.8;
    facts.focalLength = 24.0;
    refused(matchLens(facts, test::fixtureDatabase()), LensProblem::LensNotInDatabase);

    // Said after the real name, it is never reached.
    facts.lensNames = {"Sigma 18-35mm F1.8 DC HSM", "Canon EF 50mm f/1.8 STM"};
    CHECK(matched(matchLens(facts, test::fixtureDatabase())).lens.maker == "Sigma");

    // Aperture alone separates two lenses of one focal length: the 50 mm f/1.8 does not open to
    // f/4.
    LensFacts slow = on80D({"Canon EF 50mm f/1.8 STM"});
    slow.focalLength = 50.0;
    slow.maxAperture = 4.0;
    refused(matchLens(slow, test::fixtureDatabase()), LensProblem::LensNotInDatabase);
    slow.maxAperture = 1.8;
    CHECK(matched(matchLens(slow, test::fixtureDatabase())).lens.model ==
          "Canon EF 50mm f/1.8 STM");
}

TEST_CASE("Two lenses that fit equally well apply nothing", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"Sigma 17-50mm f/2.8"});
    facts.minFocal = 17.0;
    facts.maxFocal = 50.0;
    facts.maxAperture = 2.8;
    facts.focalLength = 24.0;

    const LensNoMatch none =
        refused(matchLens(facts, test::fixtureDatabase()), LensProblem::LensAmbiguous);
    REQUIRE(none.candidates.size() == 2);
    CHECK(none.candidates[0].maker == "Sigma");
    CHECK(none.candidates[0] != none.candidates[1]);

    // Said in full, one of them is the lens.
    facts.lensNames = {"Sigma 17-50mm f/2.8 EX DC HSM"};
    CHECK(matched(matchLens(facts, test::fixtureDatabase())).lens ==
          LensIdentity{"Sigma", "Sigma 17-50mm f/2.8 EX DC HSM"});
    facts.lensNames = {"Sigma 17-50mm f/2.8 EX DC OS HSM"};
    CHECK(matched(matchLens(facts, test::fixtureDatabase())).lens ==
          LensIdentity{"Sigma", "Sigma 17-50mm f/2.8 EX DC OS HSM"});
}

TEST_CASE("A lens of another mount is refused, whatever its name", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"Tamron SP 24-70mm f/2.8 Di VC USD"});
    facts.focalLength = 35.0;
    refused(matchLens(facts, test::fixtureDatabase()), LensProblem::LensNotInDatabase);

    facts.cameraMaker = "NIKON CORPORATION";
    facts.cameraModel = "NIKON D90";
    const LensMatch match = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(match.lens.maker == "Tamron");
    CHECK(match.cropFactor == Approx(1.5));
}

TEST_CASE("An unknown camera gives its crop from the focal lengths, and needs one lens left",
          "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts;
    facts.cameraMaker = "Mystery";
    facts.cameraModel = "Unlisted 1";
    facts.lensNames = {"Canon EF 50mm f/1.8 STM"};
    facts.focalLength = 50.0;
    facts.focalLength35 = 80.0;

    const LensMatch one = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK_FALSE(one.cameraListed);
    CHECK(one.cropSource == CropSource::FocalRatio);
    CHECK(one.cropFactor == Approx(1.6));

    // Two lenses fit, and without a mount nothing tells them apart, even by their full names.
    facts.lensNames = {"Sigma 17-50mm f/2.8 EX DC HSM"};
    facts.focalLength = 24.0;
    facts.focalLength35 = 38.4;
    refused(matchLens(facts, test::fixtureDatabase()), LensProblem::LensAmbiguous);

    // Without the 35 mm focal length there is no crop: the lens's own is the answer.
    facts.lensNames = {"Canon EF 50mm f/1.8 STM"};
    facts.focalLength = 50.0;
    facts.focalLength35.reset();
    const LensMatch bare = matched(matchLens(facts, test::fixtureDatabase()));
    CHECK(bare.cropSource == CropSource::Lens);
    CHECK(bare.cropFactor == Approx(1.0));
}

TEST_CASE("A lens that cannot be found says why", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();

    SECTION("a photograph that names no lens") {
        refused(matchLens(on80D({}), database), LensProblem::LensUnidentified);
        refused(matchLens(on80D({"", "  "}), database), LensProblem::LensUnidentified);
    }
    SECTION("a lens the database does not list") {
        refused(matchLens(on80D({"Canon EF 85mm f/1.2L II USM"}), database),
                LensProblem::LensNotInDatabase);
    }
    SECTION("a name too vague to name a lens") {
        LensFacts facts = on80D({"17-50mm"});
        facts.focalLength = 24.0;
        refused(matchLens(facts, database), LensProblem::LensNotInDatabase);
    }
    SECTION("a zoom used at an unrecorded focal length") {
        const LensNoMatch none =
            refused(matchLens(on80D({"EF-S18-135mm f/3.5-5.6 IS USM"}), database),
                    LensProblem::LensFocalUnknown);
        CHECK(none.candidates == std::vector<LensIdentity>{kitLens});
    }
    SECTION("a prime takes its own focal length") {
        const LensMatch match = matched(matchLens(on80D({"EF50mm f/1.8 STM"}), database));
        CHECK(match.focalLength == 50.0);
        CHECK_FALSE(match.focalLengthRead);
    }
    SECTION("a focal length outside the lens's range") {
        LensFacts facts = on80D({"EF-S18-135mm f/3.5-5.6 IS USM"});
        facts.focalLength = 200.0;
        refused(matchLens(facts, database), LensProblem::LensNotInDatabase);
    }
}

TEST_CASE("Matching against a database that did not load gives its reason", "[lens]") {
    const test::TempDir empty;
    const auto database = LensDatabase::fromDirectory(empty.path());
    const auto expected =
        lensfunAvailable() ? LensProblem::LensDatabaseMissing : LensProblem::LensfunUnavailable;
    LensFacts facts = on80D({"EF50mm f/1.8 STM"});
    refused(matchLens(facts, database), expected);
}

TEST_CASE("A RAW's lens facts are read from its EXIF", "[lensfun][lens]") {
    const ImageMetadata metadata = readImageMetadata(test::fixture("lens-canon-32x24.dng"));

    CHECK(metadata.lens.cameraMaker == "Canon");
    CHECK(metadata.lens.cameraModel == "Canon EOS 80D");
    CHECK(metadata.lens.lensNames == std::vector<std::string>{"EF-S18-135mm f/3.5-5.6 IS USM"});
    CHECK(metadata.lens.focalLength == 50.0);
    CHECK(metadata.lens.aperture == Approx(5.6));
    CHECK(metadata.lens.focalLength35 == 80.0);
    CHECK(metadata.lens.focusDistance == Approx(3.0));
    // Canon's focal range and widest aperture live in a maker note the fixture cannot carry.
    CHECK_FALSE(metadata.lens.minFocal);
    CHECK_FALSE(metadata.lens.maxFocal);

    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensMatch match = matched(matchLens(metadata.lens, test::fixtureDatabase()));
    CHECK(match.lens == kitLens);
    CHECK(match.cropFactor == Approx(1.6));
    CHECK(match.focalLength == 50.0);
}

TEST_CASE("Only a RAW carries lens facts", "[lens]") {
    const ImageMetadata metadata = readImageMetadata(test::fixture("testcard-61x41-srgb8.png"));
    CHECK(metadata.lens == LensFacts{});
}

/// Hidden: what reading the lens facts adds to describing a RAW (ADR 050, step 3). Run with
/// `arraw-tests "[.lensbench]"`; it prints, and asserts nothing.
TEST_CASE("The cost of the lens facts in describing a RAW", "[.lensbench]") {
    const auto path = test::fixture("lens-canon-32x24.dng");
    constexpr int rounds = 2000;
    const auto time = [&](auto&& work) {
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round) {
            work();
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                   .count() /
               rounds;
    };
    const double whole = time([&] { (void)readImageMetadata(path); });
    const double facts = time([&] {
        LensFacts read;
        lensfacts::complete(path, read);
    });
    WARN("readImageMetadata " << whole << " us, of which the lens facts (EXIF) " << facts << " us");
    SUCCEED();
}
