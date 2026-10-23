#include "Photo.h"

#include <DevelopState.h>

#include <stdexcept>
#include <utility>

using namespace arraw;

Photo::Photo(std::filesystem::path path, ImageMetadata metadata, NegativeSettings negative,
             DevelopState state, PhotoMarks marks)
    : path_(std::move(path)), metadata_(std::move(metadata)), negative_(negative),
      state_(std::move(state)), marks_(marks) {
    validate(negative_);
    validate(state_);
    if (marks_.rating < rejectedRating || marks_.rating > highestRating) {
        throw std::invalid_argument("rating must be from -1 to 5");
    }
}

Photo::Photo(std::filesystem::path path, ImageMetadata metadata, DevelopState state,
             PhotoMarks marks)
    : Photo(std::move(path), metadata, defaultNegativeFor(metadata), std::move(state), marks) {}

Photo::Photo(std::filesystem::path path, ImageMetadata metadata)
    : Photo(std::move(path), metadata, defaultStateFor(metadata.encoding)) {}

DecodeOptions arraw::decodeOptionsFor(const Photo& photo, bool halfSize) {
    return canonical({.halfSize = halfSize, .demosaic = photo.negative().demosaic},
                     photo.metadata());
}
