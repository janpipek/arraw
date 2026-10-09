// The local adjustments in an XMP sidecar (ADR 044, section 9).

#include "support/Fixtures.h"
#include "support/LocalAdjustmentStates.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <ImageImport.h>
#include <LocalAdjustmentEdits.h>
#include <Photo.h>
#include <SettingDescriptors.h>
#include <Sidecar.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view rawFixture = "linear-32x24-neutral.dng";

std::string slurp(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void spit(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::size_t occurrences(const std::string& text, std::string_view piece) {
    std::size_t total = 0;
    for (std::size_t at = text.find(piece); at != std::string::npos;
         at = text.find(piece, at + piece.size())) {
        ++total;
    }
    return total;
}

/// A raw fixture copied into a directory, so that a sidecar can sit beside it.
struct Workbench {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");
    fs::path sidecar = directory.file("IMG_1.xmp");

    Workbench() {
        fs::copy_file(test::fixture(rawFixture), photo);
    }

    /// Writes a state through the sidecar writer.
    void write(const DevelopState& state, PhotoMarks marks = {}) const {
        writeSidecar(openPhoto(photo).with(state).with(marks));
    }

    /// Reads the state back, with the warnings.
    DevelopState read(CollectedDiagnostics& log) const {
        const auto contents = readSidecar(photo, log);
        REQUIRE(contents);
        REQUIRE(contents->state);
        return *contents->state;
    }

    DevelopState read() const {
        CollectedDiagnostics log;
        return read(log);
    }
};

std::size_t count(const CollectedDiagnostics& log, Notice notice) {
    std::size_t total = 0;
    for (const Diagnostic& entry : log.entries()) {
        total += entry.notice == notice ? 1 : 0;
    }
    return total;
}

/// Replaces the first occurrence of a piece of text.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

std::vector<std::uint32_t> idsOf(const DevelopState& state) {
    std::vector<std::uint32_t> ids;
    for (const LocalAdjustment& adjustment : state.localAdjustments) {
        ids.push_back(adjustment.id.value);
    }
    return ids;
}

// Raw XMP, for the spellings and the damage no writer of ours produces.

constexpr std::string_view arrawNamespace = "http://ns.arraw.org/develop/1.0/";

/// One `arraw:` property element.
std::string field(std::string_view key, std::string_view value) {
    return "<arraw:" + std::string(key) + ">" + std::string(value) + "</arraw:" + std::string(key) +
           ">";
}

/// The fields of a linear mask, as child elements.
std::string linearFields(int id, const std::string& extra = "") {
    return field("id", std::to_string(id)) + field("type", "linear") + field("name", "") +
           field("enabled", "True") + field("opacity", "1") + field("invert", "False") +
           field("fromX", "0.5") + field("fromY", "0.2") + field("toX", "0.5") +
           field("toY", "0.8") + extra;
}

/// The fields of a radial mask, as child elements.
std::string radialFields(int id, const std::string& extra = "") {
    return field("id", std::to_string(id)) + field("type", "radial") + field("name", "") +
           field("enabled", "True") + field("opacity", "1") + field("invert", "False") +
           field("centreX", "0.5") + field("centreY", "0.5") + field("radiusX", "0.2") +
           field("radiusY", "0.2") + field("angle", "0") + field("feather", "0.5") + extra;
}

/// An `rdf:li` structure in the form the writer uses.
std::string item(const std::string& fields) {
    return R"(<rdf:li rdf:parseType="Resource">)" + fields + "</rdf:li>";
}

/// A sidecar with the arraw attributes given and a list of structures.
std::string sidecarWith(const std::string& attributes, const std::string& items,
                        bool withList = true) {
    return std::string(R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw=")") +
           std::string(arrawNamespace) + R"(" xmlns:xmp="http://ns.adobe.com/xap/1.0/" )" +
           attributes + ">" +
           (withList ? "<arraw:localAdjustments><rdf:Seq>" + items +
                           "</rdf:Seq></arraw:localAdjustments>"
                     : std::string()) +
           "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>\n";
}

/// Reads a hand-made sidecar.
struct Hand {
    Workbench bench;
    CollectedDiagnostics log;
    DevelopState state;

    explicit Hand(const std::string& xmp) {
        spit(bench.sidecar, xmp);
        state = bench.read(log);
    }
};

DevelopState sixteen() {
    DevelopState state;
    for (std::size_t i = 0; i < maximumLocalAdjustments; ++i) {
        LocalAdjustment adjustment;
        adjustment.name = i % 3 == 0 ? "" : "mask " + std::to_string(i);
        adjustment.opacity = 0.0625F * static_cast<float>(i);
        adjustment.invert = i % 2 == 1;
        adjustment.enabled = i % 4 != 3;
        adjustment.deltas.contrast = 0.1F * static_cast<float>(i) - 0.7F;
        adjustment.deltas.relativeTint = 1.0F / 3.0F * static_cast<float>(i);
        adjustment.shape = i % 2 == 0
                               ? Mask{LinearMask{.from = {0.1F, 0.1F * static_cast<float>(i)},
                                                 .to = {0.9F, 1.0F / 3.0F}}}
                               : Mask{RadialMask{.centre = {0.3F, 0.7F},
                                                 .radiusX = 0.1F + 0.01F * static_cast<float>(i),
                                                 .radiusY = 0.2F,
                                                 .angle = -170.0F + static_cast<float>(i),
                                                 .feather = 0.05F * static_cast<float>(i)}};
        state = withLocalAdjustmentAdded(state, adjustment);
    }
    return state;
}

} // namespace

TEST_CASE("Every kind of mask survives a write and a read", "[local][sidecar]") {
    const Workbench bench;
    const std::vector<DevelopState> states = {
        test::stateWithMasks(),
        test::stateWithEveryDelta(),
        sixteen(),
        // An empty list with a raised counter must come back raised.
        withLocalAdjustmentRemoved(withLocalAdjustmentAdded(DevelopState{}, LinearMask{}), {1}),
    };
    for (const DevelopState& state : states) {
        bench.write(state);
        CollectedDiagnostics log;
        CHECK(bench.read(log) == state);
        CHECK(log.entries().empty());
    }
    // Settings and masks together, and the marks beside them.
    DevelopState both = test::stateWithMasks();
    both.settings.tone.exposure = 0.75F;
    both.settings.color.saturation = -12.0F;
    bench.write(both, {.rating = 3, .label = ColorLabel::Green});
    const auto contents = readSidecar(bench.photo);
    REQUIRE(contents);
    CHECK(*contents->state == both);
    CHECK(contents->marks == PhotoMarks{.rating = 3, .label = ColorLabel::Green});
}

TEST_CASE("Disabled and inverted masks keep their flags", "[local][sidecar]") {
    const Workbench bench;
    const DevelopState state = test::stateWithMasks();
    REQUIRE_FALSE(state.localAdjustments[0].enabled);
    REQUIRE(state.localAdjustments[1].invert);
    bench.write(state);
    const DevelopState back = bench.read();
    CHECK_FALSE(back.localAdjustments[0].enabled);
    CHECK(back.localAdjustments[1].enabled);
    CHECK(back.localAdjustments[1].invert);
    CHECK_FALSE(back.localAdjustments[0].invert);
    CHECK(back.localAdjustments[1].opacity == 0.35F);
    CHECK(back.localAdjustments[1].name == "Sky \"left\" & <more>");
}

TEST_CASE("A name keeps its spaces and its characters", "[local][sidecar]") {
    const Workbench bench;
    DevelopState state = test::stateWithMasks();
    for (const char* name :
         {"  padded  ", "Zon\xC3\xA9 \xF0\x9F\x8C\x85", "a&b<c>d\"e'f", "", " "}) {
        state = withLocalAdjustmentRenamed(state, state.localAdjustments[0].id, name);
        bench.write(state);
        CHECK(bench.read().localAdjustments[0].name == name);
    }
}

TEST_CASE("A name XML cannot hold never reaches the sidecar", "[local][sidecar]") {
    const Workbench bench;
    const DevelopState state = test::stateWithMasks();
    bench.write(state);
    const LocalAdjustmentId id = state.localAdjustments[0].id;
    CHECK_THROWS_AS(withLocalAdjustmentRenamed(state, id, "x\xEF\xBF\xBEy"), std::invalid_argument);
    DevelopState bad = state;
    bad.localAdjustments[0].name = "x\xFFy";
    CHECK_THROWS_AS(bench.write(bad), std::invalid_argument);
    // The sidecar written before is still whole.
    CollectedDiagnostics log;
    CHECK(bench.read(log) == state);
    CHECK(log.entries().empty());
}

TEST_CASE("The list is a sequence of resource structures beside three properties",
          "[local][sidecar]") {
    const Workbench bench;
    bench.write(test::stateWithMasks());
    const std::string text = slurp(bench.sidecar);
    CHECK_THAT(text, ContainsSubstring("<arraw:localAdjustments>"));
    CHECK_THAT(text, ContainsSubstring("<rdf:Seq>"));
    CHECK(occurrences(text, R"(rdf:parseType="Resource")") == 3);
    CHECK(occurrences(text, "<arraw:id>") == 3);
    CHECK_THAT(text, ContainsSubstring("<arraw:type>linear</arraw:type>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:type>radial</arraw:type>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:enabled>False</arraw:enabled>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:invert>True</arraw:invert>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:fromX>0.2</arraw:fromX>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:centreX>0.4</arraw:centreX>"));
    CHECK_THAT(text, ContainsSubstring("<arraw:exposure>0.5</arraw:exposure>"));
    CHECK_THAT(text,
               ContainsSubstring("<arraw:relativeTemperature>-42.5</arraw:relativeTemperature>"));
    CHECK_THAT(text, ContainsSubstring(R"(arraw:localAdjustmentsVersion="1")"));
    CHECK_THAT(text, ContainsSubstring(R"(arraw:nextLocalAdjustmentId="4")"));
    // Zero deltas are not written: the first mask has two.
    CHECK(occurrences(text, "<arraw:dehaze>") == 1);
    CHECK(occurrences(text, "<arraw:contrast>") == 0);
}

TEST_CASE("A state with no masks and a fresh counter leaves no trace", "[local][sidecar]") {
    const Workbench bench;
    bench.write(test::stateWithMasks());
    REQUIRE_THAT(slurp(bench.sidecar), ContainsSubstring("localAdjustments"));
    bench.write(DevelopState{});
    const std::string text = slurp(bench.sidecar);
    CHECK_THAT(text, !ContainsSubstring("localAdjustments"));
    CHECK_THAT(text, !ContainsSubstring("nextLocalAdjustmentId"));
    CHECK(bench.read() == DevelopState{});
    // A fresh sidecar of such a state is the same.
    fs::remove(bench.sidecar);
    bench.write(DevelopState{});
    CHECK_THAT(slurp(bench.sidecar), !ContainsSubstring("localAdjustments"));
    // With a raised counter and no masks it leaves the counter and the version.
    DevelopState counted;
    counted.nextLocalAdjustmentId = LocalAdjustmentId{9};
    bench.write(counted);
    const std::string kept = slurp(bench.sidecar);
    CHECK_THAT(kept, ContainsSubstring(R"(arraw:nextLocalAdjustmentId="9")"));
    CHECK_THAT(kept, ContainsSubstring(R"(arraw:localAdjustmentsVersion="1")"));
    CHECK(bench.read() == counted);
}

TEST_CASE("Writing again rewrites the list whole and says each thing once", "[local][sidecar]") {
    const Workbench bench;
    DevelopState state = test::stateWithMasks();
    for (int round = 0; round < 3; ++round) {
        bench.write(state);
        const std::string text = slurp(bench.sidecar);
        CHECK(occurrences(text, "<arraw:localAdjustments>") == 1);
        CHECK(occurrences(text, "localAdjustmentsVersion=") == 1);
        CHECK(occurrences(text, "nextLocalAdjustmentId=") == 1);
        CHECK(occurrences(text, "<arraw:id>") == state.localAdjustments.size());
        CHECK(bench.read() == state);
        state = withLocalAdjustmentRemoved(state, state.localAdjustments.front().id);
    }
    CHECK(idsOf(bench.read()) == std::vector<std::uint32_t>{3});
}

TEST_CASE("A mask's delta is never read as the global setting of the same name",
          "[local][sidecar]") {
    const Workbench bench;
    DevelopState state = test::stateWithMasks();
    REQUIRE(state.localAdjustments[0].deltas.exposure == 0.5F);
    state.settings.tone.exposure = -1.0F;
    bench.write(state);
    const DevelopState back = bench.read();
    CHECK(back.settings.tone.exposure == -1.0F);
    CHECK(back.localAdjustments[0].deltas.exposure == 0.5F);
    // Nor the other way round: a global attribute is not a mask's.
    CHECK(back == state);
}

TEST_CASE("The three properties are known, not reported as unknown settings", "[local][sidecar]") {
    const Workbench bench;
    bench.write(test::stateWithMasks());
    CollectedDiagnostics log;
    (void)readSidecar(bench.photo, log);
    CHECK(count(log, Notice::SettingUnknown) == 0);
    CHECK(log.entries().empty());
    // To a build that does not know them they are unknown keys, which it keeps when it writes.
    for (const char* key :
         {"localAdjustments", "localAdjustmentsVersion", "nextLocalAdjustmentId"}) {
        CHECK(findDescriptor(key) == nullptr);
    }
    // A key nobody knows is still reported.
    Hand hand(sidecarWith(R"(arraw:futureKnob="7")", item(linearFields(1))));
    CHECK(count(hand.log, Notice::SettingUnknown) == 1);
}

TEST_CASE("A sidecar that holds only the list records a state", "[local][sidecar]") {
    Hand hand(sidecarWith("", item(linearFields(4))));
    CHECK(idsOf(hand.state) == std::vector<std::uint32_t>{4});
    CHECK(hand.state.nextLocalAdjustmentId == LocalAdjustmentId{5});
    CHECK(hand.state.settings == DevelopSettings{});
    CHECK(hand.log.entries().empty());

    // A counter alone records one too, with no masks.
    Hand counter(sidecarWith(R"(arraw:nextLocalAdjustmentId="12")", "", false));
    CHECK(counter.state.localAdjustments.empty());
    CHECK(counter.state.nextLocalAdjustmentId == LocalAdjustmentId{12});

    // A sidecar of marks alone still records none.
    const Workbench bench;
    writeSidecarMarks(bench.photo, {.rating = 2});
    CHECK_FALSE(readSidecar(bench.photo)->state);
}

TEST_CASE("The alternative spellings of RDF read alike", "[local][sidecar]") {
    const DevelopState expected = [] {
        DevelopState state = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
        state.localAdjustments[0].id = LocalAdjustmentId{1};
        state.localAdjustments[0].shape = LinearMask{.from = {0.5F, 0.2F}, .to = {0.5F, 0.8F}};
        state.localAdjustments[0].deltas.exposure = 0.5F;
        state.localAdjustments[0].invert = true;
        state.localAdjustments[0].name = "Sky";
        state.nextLocalAdjustmentId = LocalAdjustmentId{2};
        return state;
    }();
    const std::string asElements =
        field("id", "1") + field("type", "linear") + field("name", "Sky") +
        field("enabled", "True") + field("opacity", "1") + field("invert", "True") +
        field("fromX", "0.5") + field("fromY", "0.2") + field("toX", "0.5") + field("toY", "0.8") +
        field("exposure", "0.5");
    const std::string asAttributes =
        R"(arraw:id="1" arraw:type="linear" arraw:name="Sky" arraw:enabled="True" arraw:opacity="1")"
        R"( arraw:invert="True" arraw:fromX="0.5" arraw:fromY="0.2" arraw:toX="0.5" arraw:toY="0.8")"
        R"( arraw:exposure="0.5")";

    SECTION("parseType Resource, fields as elements") {
        Hand hand(sidecarWith("", item(asElements)));
        CHECK(hand.state == expected);
        CHECK(hand.log.entries().empty());
    }
    SECTION("fields as attributes of the item") {
        Hand hand(sidecarWith("", "<rdf:li " + asAttributes + "/>"));
        CHECK(hand.state == expected);
        CHECK(hand.log.entries().empty());
    }
    SECTION("a nested description with fields as elements") {
        Hand hand(sidecarWith("", "<rdf:li><rdf:Description>" + asElements +
                                      "</rdf:Description></rdf:li>"));
        CHECK(hand.state == expected);
    }
    SECTION("a nested description with fields as attributes") {
        Hand hand(sidecarWith("", "<rdf:li><rdf:Description " + asAttributes + "/></rdf:li>"));
        CHECK(hand.state == expected);
    }
    SECTION("a mixture, with spaces around the numbers") {
        Hand hand(sidecarWith(
            "", R"(<rdf:li arraw:id=" 1 " arraw:type="linear"><arraw:name>Sky</arraw:name>)"
                R"(<arraw:invert>true</arraw:invert><arraw:fromX> 0.5</arraw:fromX>)"
                R"(<arraw:fromY>0.2 </arraw:fromY><arraw:toX>+0.5</arraw:toX>)"
                R"(<arraw:toY>0.8</arraw:toY><arraw:exposure>5e-1</arraw:exposure></rdf:li>)"));
        CHECK(hand.state == expected);
    }
    SECTION("another prefix for the namespace, and the list in another description") {
        const std::string xmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:a="http://ns.arraw.org/develop/1.0/" a:nextLocalAdjustmentId="2"/>
  <rdf:Description rdf:about="" xmlns:q="http://ns.arraw.org/develop/1.0/">
   <q:localAdjustments><rdf:Seq><rdf:li rdf:parseType="Resource">
    <q:id>1</q:id><q:type>linear</q:type><q:name>Sky</q:name><q:enabled>True</q:enabled>
    <q:opacity>1</q:opacity><q:invert>True</q:invert><q:fromX>0.5</q:fromX><q:fromY>0.2</q:fromY>
    <q:toX>0.5</q:toX><q:toY>0.8</q:toY><q:exposure>0.5</q:exposure>
   </rdf:li></rdf:Seq></q:localAdjustments>
  </rdf:Description>
 </rdf:RDF></x:xmpmeta>)";
        Hand hand(xmp);
        CHECK(hand.state == expected);
        CHECK(hand.log.entries().empty());
    }
    SECTION("a rdf prefix other than rdf") {
        const std::string xmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <r:RDF xmlns:r="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <r:Description r:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/">
   <arraw:localAdjustments><r:Seq><r:li r:parseType="Resource">)" +
                                asElements + R"(</r:li></r:Seq></arraw:localAdjustments>
  </r:Description></r:RDF></x:xmpmeta>)";
        Hand hand(xmp);
        CHECK(hand.state == expected);
    }
}

TEST_CASE("A mask's fields are not top-level settings", "[local][sidecar]") {
    // Only the description's own properties are settings; a field inside a structure is not.
    Hand hand(
        sidecarWith("", item(linearFields(1, field("exposure", "3") + field("contrast", "40")))));
    CHECK(hand.state.settings.tone.exposure == 0.0F);
    CHECK(hand.state.settings.tone.contrast == 0.0F);
    CHECK(hand.state.localAdjustments[0].deltas.exposure == 3.0F);
    CHECK(hand.state.localAdjustments[0].deltas.contrast == 40.0F);
    CHECK(count(hand.log, Notice::SettingUnknown) == 0);
}

TEST_CASE("Entries that cannot be used are dropped with a warning that names them",
          "[local][sidecar]") {
    const std::vector<std::string> bad = {
        item(field("id", "2") + field("type", "brush") + field("rasteriser", "1") +
             field("strokes", "")),
        item(field("id", "2") + field("type", "spiral")),
        item(field("type", "linear") + field("fromX", "0") + field("fromY", "0") +
             field("toX", "1") + field("toY", "1")), // no id
        item(field("id", "zero") + field("type", "linear")),
        item(field("id", "0") + field("type", "linear")),
        item(field("id", "2.5") + field("type", "linear")),
        item(field("id", "2")), // no type
        item(field("id", "2") + field("type", "linear") + field("fromX", "0") +
             field("fromY", "0")),
        item(replaced(linearFields(2), "<arraw:fromX>0.5", "<arraw:fromX>x")),
        item(linearFields(2, field("enabled", "maybe"))),
        item(linearFields(2, field("opacity", "half"))),
        item(linearFields(2, field("exposure", "lots"))),
        item(linearFields(2, "<arraw:exposure><arraw:inner>1</arraw:inner></arraw:exposure>")),
        item(replaced(linearFields(2), "<arraw:toY>0.8", "<arraw:toY>nan")),
        item(replaced(linearFields(2), "<arraw:toY>0.8", "<arraw:toY>inf")),
        item(linearFields(2, field("exposure", "-inf"))),
        "<rdf:li>just text</rdf:li>",
        "<rdf:li/>",
        // Degenerate geometry.
        item(field("id", "2") + field("type", "linear") + field("fromX", "0.5") +
             field("fromY", "0.5") + field("toX", "0.5") + field("toY", "0.5")),
        item(replaced(radialFields(2), "<arraw:radiusX>0.2<", "<arraw:radiusX>0<")),
    };
    for (std::size_t i = 0; i < bad.size(); ++i) {
        INFO(i << ": " << bad[i]);
        Hand hand(sidecarWith("", item(linearFields(1)) + bad[i] + item(radialFields(3))));
        CHECK(idsOf(hand.state) == std::vector<std::uint32_t>{1, 3});
        REQUIRE(count(hand.log, Notice::LocalAdjustmentDropped) == 1);
        for (const Diagnostic& entry : hand.log.entries()) {
            if (entry.notice == Notice::LocalAdjustmentDropped) {
                CHECK(std::get<double>(entry.values[0]) == 2.0);
                CHECK(entry.subject == hand.bench.photo);
                CHECK_FALSE(std::get<std::string>(entry.values[3]).empty());
            }
        }
    }
}

TEST_CASE("The dropped entries are named by id and type when they have them", "[local][sidecar]") {
    Hand hand(sidecarWith("", item(linearFields(1)) +
                                  item(field("id", "7") + field("type", "brush")) +
                                  item(linearFields(1))));
    REQUIRE(count(hand.log, Notice::LocalAdjustmentDropped) == 2);
    const Diagnostic& brush = hand.log.entries()[0];
    CHECK(std::get<std::string>(brush.values[1]) == "7");
    CHECK(std::get<std::string>(brush.values[2]) == "brush");
    CHECK_THAT(describe(brush), ContainsSubstring("mask 2 (id 7, brush)"));
    const Diagnostic& twin = hand.log.entries()[1];
    CHECK(std::get<double>(twin.values[0]) == 3.0);
    CHECK_THAT(describe(twin), ContainsSubstring("id 1"));
    CHECK(idsOf(hand.state) == std::vector<std::uint32_t>{1});
}

TEST_CASE("Entries past sixteen are dropped", "[local][sidecar]") {
    std::string items;
    for (int id = 1; id <= 18; ++id) {
        items += item(id % 2 == 0 ? radialFields(id) : linearFields(id));
    }
    Hand hand(sidecarWith("", items));
    CHECK(hand.state.localAdjustments.size() == 16);
    CHECK(count(hand.log, Notice::LocalAdjustmentDropped) == 2);
    CHECK(hand.state.nextLocalAdjustmentId == LocalAdjustmentId{17});
    CHECK_NOTHROW(validate(hand.state));
}

TEST_CASE("Unknown fields are ignored with a warning, numbers out of range clamped",
          "[local][sidecar]") {
    Hand hand(sidecarWith(
        "", item(radialFields(1, field("sparkle", "3") + field("filmicHighlights", "1") +
                                     field("exposure", "9") + field("opacity", "2"))) +
                item(linearFields(2, field("centreX", "0.5")))));
    CHECK(idsOf(hand.state) == std::vector<std::uint32_t>{1, 2});
    CHECK(count(hand.log, Notice::LocalAdjustmentFieldIgnored) == 3);
    CHECK(count(hand.log, Notice::LocalAdjustmentDropped) == 0);
    // The later opacity wins, and is brought into range; so is the exposure.
    CHECK(hand.state.localAdjustments[0].opacity == 1.0F);
    CHECK(hand.state.localAdjustments[0].deltas.exposure == 4.0F);
    CHECK(count(hand.log, Notice::SettingClamped) == 2);
    CHECK_NOTHROW(validate(hand.state));
}

TEST_CASE("The counter is repaired on reading", "[local][sidecar]") {
    const std::string items = item(linearFields(2)) + item(radialFields(5));
    CHECK(Hand(sidecarWith("", items)).state.nextLocalAdjustmentId == LocalAdjustmentId{6});
    CHECK(Hand(sidecarWith(R"(arraw:nextLocalAdjustmentId="3")", items))
              .state.nextLocalAdjustmentId == LocalAdjustmentId{6});
    CHECK(Hand(sidecarWith(R"(arraw:nextLocalAdjustmentId="30")", items))
              .state.nextLocalAdjustmentId == LocalAdjustmentId{30});
    for (const char* bad :
         {R"(arraw:nextLocalAdjustmentId="many")", R"(arraw:nextLocalAdjustmentId="0")",
          R"(arraw:nextLocalAdjustmentId="2.5")"}) {
        INFO(bad);
        Hand hand(sidecarWith(bad, items));
        CHECK(hand.state.nextLocalAdjustmentId == LocalAdjustmentId{6});
        CHECK(count(hand.log, Notice::SettingMalformed) == 1);
        CHECK(hand.state.localAdjustments.size() == 2);
    }
}

TEST_CASE("A list that is not a sequence is reported and read as empty", "[local][sidecar]") {
    Hand attribute(sidecarWith(R"(arraw:localAdjustments="nothing")", "", false));
    CHECK(attribute.state.localAdjustments.empty());
    CHECK(count(attribute.log, Notice::SettingMalformed) == 1);

    const std::string bag = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/">
   <arraw:localAdjustments><rdf:Bag/></arraw:localAdjustments>
  </rdf:Description></rdf:RDF></x:xmpmeta>)";
    Hand hand(bag);
    CHECK(hand.state.localAdjustments.empty());
    CHECK(count(hand.log, Notice::SettingMalformed) == 1);

    Hand empty(sidecarWith("", ""));
    CHECK(empty.state.localAdjustments.empty());
    CHECK(empty.log.entries().empty());
}

TEST_CASE("A newer list version is read, and then the sidecar is not written back",
          "[local][sidecar]") {
    Workbench bench;
    const std::string newer = sidecarWith(R"(arraw:localAdjustmentsVersion="2")",
                                          item(linearFields(1, field("futureThing", "1"))));
    spit(bench.sidecar, newer);
    CollectedDiagnostics log;
    const DevelopState state = bench.read(log);
    CHECK(state.localAdjustments.size() == 1);
    REQUIRE(count(log, Notice::NewerLocalAdjustmentsVersion) == 1);
    CHECK(count(log, Notice::LocalAdjustmentFieldIgnored) == 1);

    CHECK_THROWS_WITH(bench.write(state), ContainsSubstring("newer"));
    CHECK_THROWS_AS(writeSidecarMarks(bench.photo, {.rating = 4}), std::runtime_error);
    CHECK(slurp(bench.sidecar) == newer);

    // An unreadable version is not a newer one.
    spit(bench.sidecar,
         sidecarWith(R"(arraw:localAdjustmentsVersion="one")", item(linearFields(1))));
    CollectedDiagnostics unreadable;
    CHECK(bench.read(unreadable).localAdjustments.size() == 1);
    CHECK(count(unreadable, Notice::SettingMalformed) == 1);
    CHECK_NOTHROW(bench.write(state));
}

TEST_CASE("Writing the marks leaves the list as it is", "[local][sidecar]") {
    // What a build that does not know the list does with it: it sets the properties it knows and
    // leaves the rest. Writing only the marks is the same case in this build.
    const Workbench bench;
    bench.write(test::stateWithMasks());
    const DevelopState before = bench.read();
    writeSidecarMarks(bench.photo, {.rating = 5, .label = ColorLabel::Red});
    CHECK(bench.read() == before);
    CHECK(readSidecar(bench.photo)->marks == PhotoMarks{.rating = 5, .label = ColorLabel::Red});
}

TEST_CASE("Foreign properties survive a write of the list", "[local][sidecar]") {
    const Workbench bench;
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), bench.sidecar);
    const auto before = readSidecar(bench.photo);
    REQUIRE(before);
    REQUIRE(before->state);

    DevelopState state = *before->state;
    state = withLocalAdjustmentAdded(state, test::someLinear());
    state = withLocalDelta(state, state.localAdjustments[0].id, "clarity", 25.0);
    writeSidecar(openPhoto(bench.photo).with(state).with(before->marks));
    const auto after = readSidecar(bench.photo);
    REQUIRE(after);
    CHECK(*after->state == state);
    CHECK(after->others == before->others);
    CHECK(after->creatorTool == before->creatorTool);
    CHECK(after->marks == before->marks);
    const std::string text = slurp(bench.sidecar);
    for (const char* piece :
         {"crs:Exposure2012", "acme:Keywords", "harbour", "dc:creator", "futureKnob"}) {
        INFO(piece);
        CHECK_THAT(text, ContainsSubstring(piece));
    }
    // Removing the masks again removes the list and nothing else.
    writeSidecar(openPhoto(bench.photo).with(*before->state).with(before->marks));
    const std::string clean = slurp(bench.sidecar);
    CHECK_THAT(clean, !ContainsSubstring("localAdjustments"));
    CHECK_THAT(clean, ContainsSubstring("harbour"));
}

TEST_CASE("A photograph opens with the masks of its sidecar", "[local][sidecar]") {
    const Workbench bench;
    const DevelopState state = test::stateWithMasks();
    bench.write(state);
    CHECK(openPhoto(bench.photo).state() == state);
}
