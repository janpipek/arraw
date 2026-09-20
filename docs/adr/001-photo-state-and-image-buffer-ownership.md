# Photos own editable state; processors own pixel buffers

Opening and developing a photograph produces information at different times:
file associations are discovered first, metadata and sidecars are resolved when
the document opens, and decoded or developed pixels are produced on demand. A
partially populated photo would make absence, loading, and failure ambiguous.

## Decision

`Shot` describes a primary image and its secondary images. `Photo` is created
only when that shot's metadata, sidecar binding, and editable develop state form
a coherent document. Loading and failure states belong to the editing session;
decoded pixels do not belong to `Photo`.

`ImageBuffer` owns one tightly packed CPU colour raster, including its size,
pixel format, and colour encoding. It is movable but not copyable. Processing
may mutate a private buffer while building a result; published and cached
buffers are shared as `std::shared_ptr<const ImageBuffer>`.

Processors own decoded and derived pixel caches. CPU images and GPU textures
remain separate backend resources, although both are produced from the same
photo snapshot and processing contract. RAW mosaics and masks use distinct
types rather than pretending to be colour images.

## Consequences

- The CLI and GUI pass the same coherent `Photo` values to processing.
- Export keeps a photo snapshot while later GUI edits create newer revisions.
- Cache eviction can release its reference without invalidating an active render.
- Cache keys and memory budgets remain processing concerns.

