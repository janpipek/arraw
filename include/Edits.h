#pragma once

#include <DevelopState.h>
#include <EffectsSettings.h>
#include <GeometrySettings.h>
#include <ImageImport.h>
#include <SettingDescriptors.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace arraw {

namespace detail {

/// @brief Whether a type is a number a numeric setting takes: any arithmetic type but `bool`.
template <class T>
concept SettingNumber = std::is_arithmetic_v<T> && !std::same_as<T, bool>;

/// @brief Whether a type is a `std::optional`.
template <class T> inline constexpr bool isOptional = false;

/// @copydoc isOptional
template <class U> inline constexpr bool isOptional<std::optional<U>> = true;

/// @brief Tells whether a field of type @p Field takes a value of type @p T: the
/// type-matching rule of ::arraw::withValue.
///
/// - `float` and `double` fields take any ::arraw::detail::SettingNumber;
/// - the `std::uint32_t` grain seed takes any integral type but `bool`, since a
///   seed names a pattern and is never a fraction;
/// - an optional field takes what its value type takes, `std::nullopt`, or an
///   optional of something its value type takes, an empty one clearing it;
/// - ::arraw::CropAspect takes itself or any one of its alternatives;
/// - every other field (`bool`, the enumerations, ::arraw::ToneCurve,
///   ::arraw::UprightCropRect) takes exactly its own type, so an `int` does not
///   set a flip and a number does not name an enumerator.
template <class Field, class T> consteval bool takes() {
    if constexpr (std::same_as<Field, float> || std::same_as<Field, double>) {
        return SettingNumber<T>;
    } else if constexpr (std::same_as<Field, std::uint32_t>) {
        return std::integral<T> && !std::same_as<T, bool>;
    } else if constexpr (isOptional<Field>) {
        if constexpr (std::same_as<T, std::nullopt_t>) {
            return true;
        } else if constexpr (isOptional<T>) {
            return takes<typename Field::value_type, typename T::value_type>();
        } else {
            return takes<typename Field::value_type, T>();
        }
    } else if constexpr (std::same_as<Field, CropAspect>) {
        return std::same_as<T, CropAspect> || std::same_as<T, FreeCropAspect> ||
               std::same_as<T, OriginalCropAspect> || std::same_as<T, CropRatio>;
    } else {
        return std::same_as<T, Field>;
    }
}

/// @brief Tells whether some leaf type of ::arraw::SettingAccessor takes a value of type @p T.
template <class T, class Accessor> struct SomeSettingTakes;

/// @copydoc SomeSettingTakes
template <class T, class... Accessors>
struct SomeSettingTakes<T, std::variant<Accessors...>>
    : std::bool_constant<(
          takes<std::remove_reference_t<std::invoke_result_t<Accessors, DevelopSettings&>>, T>() ||
          ...)> {};

/// @brief Value type some setting takes, the only types ::arraw::withValue compiles for.
///
/// Derived from ::arraw::SettingAccessor, so a new leaf type is covered without
/// a change here.
template <class T>
concept SettingValue = SomeSettingTakes<T, SettingAccessor>::value;

/// @brief Finds the row of the setting a key names.
/// @param key camelCase name of the setting, such as "exposure".
/// @return The row of ::arraw::developSettingDescriptors.
/// @throws std::invalid_argument if no setting has that key.
[[nodiscard]] const FieldDescriptor& editedDescriptor(std::string_view key);

/// @brief Checks a number against the row it is for, before it is converted to the field's type.
/// @param descriptor Row of the setting.
/// @param value The number, widened to `double`.
/// @throws std::invalid_argument naming the key, the value and the range if @p value is not
/// finite or lies outside the row's range.
void checkNumber(const FieldDescriptor& descriptor, double value);

/// @brief Refuses a value whose type the setting does not take.
/// @param descriptor Row of the setting.
/// @throws std::invalid_argument naming the key and what it takes (::arraw::expectation's words).
[[noreturn]] void refuseType(const FieldDescriptor& descriptor);

/// @brief Converts a value into a field, under the rule of ::arraw::detail::takes.
/// @param descriptor Row of the setting, for the range and the messages.
/// @param field Field to set.
/// @param value Value to give it.
/// @throws std::invalid_argument if the field does not take @p T, or a number is not finite or
/// is out of range; @p field is then unchanged.
template <class Field, class T>
void assignSetting(const FieldDescriptor& descriptor, Field& field, const T& value) {
    if constexpr (!takes<Field, T>()) {
        refuseType(descriptor);
    } else if constexpr (SettingNumber<Field>) {
        // Checked as a double first: converting a number a float cannot hold is undefined.
        checkNumber(descriptor, static_cast<double>(value));
        field = static_cast<Field>(value);
    } else if constexpr (isOptional<Field> && std::same_as<T, std::nullopt_t>) {
        field.reset();
    } else if constexpr (isOptional<Field> && isOptional<T>) {
        if (value) {
            assignSetting(descriptor, field, *value);
        } else {
            field.reset();
        }
    } else if constexpr (isOptional<Field>) {
        typename Field::value_type inner{};
        assignSetting(descriptor, inner, value);
        field = inner;
    } else {
        field = value;
    }
}

/// @brief Sets the field a row describes to the value @p values holds, with its key's rule.
///
/// What ::arraw::withValue and ::arraw::withValueFrom share: the value is taken
/// from @p values as it stands, checked as ::arraw::validate checks it, and
/// applied to @p state by the rule of its key.
/// @param photo Photograph the state belongs to.
/// @param state State to edit.
/// @param descriptor Row of the setting.
/// @param values Settings holding the new value.
/// @param entropy Where a new grain seed's bits come from; empty for `std::random_device`.
/// @return The edited state.
/// @throws std::invalid_argument as ::arraw::withValue.
[[nodiscard]] DevelopState withField(const ImageMetadata& photo, DevelopState state,
                                     const FieldDescriptor& descriptor,
                                     const DevelopSettings& values, const GrainEntropy& entropy);

} // namespace detail

/// @brief Sets one setting, and whatever its key's rule changes with it.
///
/// The one way the GUI, the command line and (later) Python change a setting,
/// so that all three apply an edit by the same rules (looks-and-history plan,
/// §2). Most keys are a plain assignment after validation; the few that carry
/// a rule have it written beside the key in Edits.cpp:
///
/// - `temperature` or `tint`: a value makes the white balance Custom; clearing
///   both (`std::nullopt`) returns it to As Shot. In As Shot, a value left over
///   in the other half is dropped first, so it is not adopted.
/// - `whiteBalance`: As Shot clears the temperature and tint. Custom keeps the
///   values there are, dropping leftovers when coming from As Shot; Custom with
///   neither value renders as As Shot.
/// - `grainAmount`: going from zero to above zero gives grain that has no seed
///   a new one, drawn from @p entropy; ::arraw::chooseGrainSeed decides.
/// - `rotation`, `flipHorizontal`, `flipVertical`, `straighten`,
///   `cropRectangle`, `cropAspect`: **no rule yet**, plain assignment.
///
/// **The geometry keys will change.** For now a turn, a flip or a straighten
/// leaves an explicit crop where it is in upright coordinates, so it no longer
/// selects the same content, and a crop is not fitted back into valid content.
/// The rules of ADR 014 (a turn or flip carries the crop, a straighten shrinks
/// it, a crop is fitted to the frame) arrive with `CropGeometry.h` (plan step 1)
/// and will be added here without changing a signature: that is what @p photo
/// is for. The straighten is the stored value, before the flips, not the one
/// shown on screen.
///
/// **Types.** The value's type must be one the setting takes
/// (::arraw::detail::takes): any number for a numeric setting; for the
/// temperature and tint also `std::nullopt` or an optional; the exact type
/// otherwise. A type no setting takes does not compile.
///
/// **Validation.** Only the edited setting is checked, not the rest of the
/// state, by the checks of ::arraw::validate. Whether the setting applies to
/// @p photo (::arraw::Applicability) is not checked: a temperature on a
/// photograph that is not a RAW is stored, and the render refuses it. The
/// result is a new state, so a throw leaves the caller's state as it was.
///
/// **Grain seeds.** Left empty, @p entropy is `std::random_device`; a test
/// passes a function returning fixed bits, as with ::arraw::chooseGrainSeed.
///
/// @tparam T Type of the value; see ::arraw::detail::SettingValue.
/// @param photo Photograph the state belongs to, as ::arraw::readImageMetadata describes it;
/// no rule reads it yet.
/// @param state State to edit.
/// @param key camelCase name of the setting, as in ::arraw::developSettingDescriptors.
/// @param value New value.
/// @param entropy Where a new grain seed's bits come from; empty for `std::random_device`.
/// @return @p state with the setting changed and its rule applied.
/// @throws std::invalid_argument if @p key names no setting, the setting does not take a @p T,
/// a number is not finite or lies outside the setting's range, or a curve, crop rectangle or
/// crop ratio is not well formed.
template <detail::SettingValue T>
[[nodiscard]] DevelopState withValue(const ImageMetadata& photo, DevelopState state,
                                     std::string_view key, const T& value,
                                     const GrainEntropy& entropy = {}) {
    const FieldDescriptor& descriptor = detail::editedDescriptor(key);
    DevelopSettings values = state.settings;
    visitField(descriptor, values,
               [&](auto& field) { detail::assignSetting(descriptor, field, value); });
    return detail::withField(photo, std::move(state), descriptor, values, entropy);
}

/// @brief Sets one setting to the value another set of settings holds, with its key's rule.
///
/// The twin of ::arraw::withValue for a front end that already holds the value
/// in a ::arraw::DevelopSettings: the command line's flags, a pasted look, a
/// preset. The value is validated as ::arraw::withValue validates it, so the
/// source need not be valid elsewhere.
///
/// The temperature and tint are read as @p source's white balance means them:
/// absent when @p source is As Shot, whatever it has left in them. Copying
/// either from an As Shot source therefore clears it, and a front end that
/// wants a source's temperature copied makes that source Custom.
/// @param photo Photograph the state belongs to; reserved for the geometry rules.
/// @param state State to edit.
/// @param key camelCase name of the setting.
/// @param source Settings holding the value.
/// @param entropy Where a new grain seed's bits come from; empty for `std::random_device`.
/// @return @p state with the setting copied and its rule applied.
/// @throws std::invalid_argument as ::arraw::withValue.
[[nodiscard]] DevelopState withValueFrom(const ImageMetadata& photo, DevelopState state,
                                         std::string_view key, const DevelopSettings& source,
                                         const GrainEntropy& entropy = {});

/// @brief Sets several settings from another set of settings, each with its key's rule.
///
/// The settings are applied one by one as ::arraw::withValueFrom applies them,
/// in the order of ::arraw::developSettingDescriptors and not in the order
/// given. The table puts the rotation and the flips before the straighten and
/// the crop last, so once the geometry rules exist a carried crop is placed on
/// the final frame, and a crop named in @p keys replaces the carried one; the
/// white balance mode comes before its temperature and tint, and the grain's
/// amount before its seed, so a seed named in @p keys wins over one the amount
/// chose. A key given twice is applied once.
///
/// Every key is looked up before anything is applied. What ::arraw::withLook
/// builds on: it passes the Look-scoped keys of the chosen
/// ::arraw::CopySection values.
/// @param photo Photograph the state belongs to; reserved for the geometry rules.
/// @param state State to edit.
/// @param keys camelCase names of the settings to set, in any order.
/// @param source Settings holding the values.
/// @param entropy Where a new grain seed's bits come from; empty for `std::random_device`.
/// @return @p state with every named setting copied and its rule applied.
/// @throws std::invalid_argument as ::arraw::withValue, for the first key that fails.
[[nodiscard]] DevelopState withValues(const ImageMetadata& photo, DevelopState state,
                                      std::span<const std::string_view> keys,
                                      const DevelopSettings& source,
                                      const GrainEntropy& entropy = {});

/// @brief Copy sections ::arraw::withLook carries, in enumeration order.
///
/// Every section but ::arraw::CopySection::RotateAndFlip and
/// ::arraw::CopySection::Crop, which wait for the geometry rules (looks-and-history
/// plan, step 1): carried by plain assignment, a crop would no longer frame the
/// same content on a photograph of another size or orientation. When the rules
/// arrive both are added here, and the sections a copy dialog offers follow.
inline constexpr auto copyableSections = std::to_array<CopySection>({
    CopySection::WhiteBalance,
    CopySection::Exposure,
    CopySection::Tone,
    CopySection::Presence,
    CopySection::Color,
    CopySection::ToneCurve,
    CopySection::Hsl,
    CopySection::BlackAndWhite,
    CopySection::ColorGrading,
    CopySection::NoiseReduction,
    CopySection::Vignette,
    CopySection::Grain,
});

/// @brief Copy sections chosen until someone chooses otherwise, in enumeration order.
///
/// Every section of ::arraw::copyableSections; the geometry stays out of the
/// default even once it can be carried, as in Lightroom, because a crop rarely
/// suits another frame (looks-and-history plan, §3).
inline constexpr auto defaultCopySections = copyableSections;

/// @brief Settings taken from one photograph, to be carried onto others.
///
/// What a copy holds, and later what a preset reads into: all of the
/// settings, of which ::arraw::withLook takes the chosen sections, and the one
/// thing about the photograph they came from that decides which sections can
/// cross, whether it was a RAW (ADR 008).
struct Look {
    /// @brief Settings of the photograph the look was taken from.
    DevelopSettings settings{};

    /// @brief Whether the photograph the look was taken from is a RAW, its encoding its camera's
    /// own.
    bool fromRaw = false;

    friend bool operator==(const Look&, const Look&) = default;
};

/// @brief Takes the look of a photograph.
///
/// A RAW is a photograph whose ::arraw::ImageMetadata::encoding is not a
/// ::arraw::NamedEncoding, as everywhere else in arraw.
/// @param photo Photograph the settings belong to.
/// @param settings Its settings, usually those of its current state.
/// @return The settings, with whether @p photo is a RAW.
[[nodiscard]] Look lookOf(const ImageMetadata& photo, const DevelopSettings& settings);

/// @brief Outcome of carrying a look onto a photograph: the new state, and what was left out.
struct AppliedLook {
    /// @brief State with the look's sections carried onto it.
    DevelopState state;

    /// @brief Chosen sections not carried in full, because some of their settings apply to
    /// only one of the two photographs; in enumeration order, each once.
    ///
    /// Today only ::arraw::CopySection::WhiteBalance, between a RAW and a
    /// photograph that is not one. A front end tells the user: nothing was
    /// wrong, but something they chose did not arrive.
    std::vector<CopySection> skipped;

    friend bool operator==(const AppliedLook&, const AppliedLook&) = default;
};

/// @brief Carries chosen sections of a look onto a photograph's state.
///
/// The one function behind pasting settings and applying a preset
/// (looks-and-history plan, §3). Every ::arraw::SettingScope::Look setting of
/// each chosen section is copied from @p look through ::arraw::withValues, so
/// the rules of ::arraw::withValue apply as if each had been set by hand:
///
/// - **A section is replaced, not merged.** All of its Look settings take the
///   look's values, defaults included: a look with the tone curves flat
///   flattens the target's curves.
/// - **The photograph's own settings stay.** ::arraw::SettingScope::Photo
///   settings (the grain seed) are never copied, whatever the sections.
/// - **Grain turned on gets a seed.** When the look's grain amount turns the
///   target's grain on and the target has no seed, one is drawn from
///   @p entropy (::arraw::chooseGrainSeed); a target that had a seed keeps it,
///   so its pattern does not change.
/// - **Settings that apply on one side only do not cross (ADR 008).** A
///   setting is copied only when it applies (::arraw::Applicability) both to
///   the photograph the look came from and to @p photo. A chosen section that
///   loses a setting because it applies on one side and not the other is
///   listed in AppliedLook::skipped, and the target keeps its own values of
///   what was not copied. Today this means the white balance does not cross
///   between a RAW and a photograph that is not one; between two that are
///   not RAWs it applies to neither, is not copied, and is not reported.
/// - **The white balance is carried as the look's mode means it.** A look in
///   As Shot gives As Shot, whatever it has left in its temperature and tint
///   (::arraw::withValueFrom). Between two RAWs, As Shot means each camera's
///   own reading, not the source's Kelvin.
///
/// A section given twice is applied once, and the order of @p sections does
/// not matter. An empty @p sections leaves the state as it was, with nothing skipped.
///
/// **Geometry.** ::arraw::CopySection::RotateAndFlip and
/// ::arraw::CopySection::Crop are refused until the geometry rules are in core
/// (see ::arraw::copyableSections), so no caller gets a crop that was quietly
/// not fitted to the target's frame.
///
/// **Errors.** Every section is checked before anything is applied, and the
/// result is a new state, so a throw leaves the caller's state as it was. The
/// look's values are validated as ::arraw::withValue validates them, so a look
/// read from a damaged file is refused rather than pasted.
/// @param photo Photograph the state belongs to, as ::arraw::readImageMetadata describes it:
/// whether it is a RAW decides what crosses, and the geometry rules will need its frame.
/// @param state State to carry the look onto.
/// @param look Settings to carry, and whether they came from a RAW.
/// @param sections Sections of @p look to carry, in any order.
/// @param entropy Where a new grain seed's bits come from; empty for `std::random_device`.
/// @return @p state with the sections carried onto it, and the sections left out.
/// @throws std::invalid_argument naming the section if @p sections holds one not in
/// ::arraw::copyableSections; as ::arraw::withValue if a value of @p look is not valid.
[[nodiscard]] AppliedLook withLook(const ImageMetadata& photo, DevelopState state, const Look& look,
                                   std::span<const CopySection> sections,
                                   const GrainEntropy& entropy = {});

} // namespace arraw
