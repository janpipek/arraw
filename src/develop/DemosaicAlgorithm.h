#pragma once

#include <cstdint>
#include <QString>

// Per-image demosaic algorithm selection (issue #22, docs/adr/0036).
//
// This header is the single source of truth tying the libraw built-in demosaic
// set to its persisted string token and its libraw `user_qual` integer. The
// sidecar, the decode-cache key, and RawProcessor all read from here, so the
// stored token never silently changes meaning if libraw renumbers.
//
// Only libraw's LGPL built-ins are exposed (no GPL demosaic-pack). AMaZE/RCD and
// an X-Trans Markesteijn menu are deferred (see ADR 0036).
enum class DemosaicAlgorithm : std::uint8_t { AHD, VNG, PPG, DCB, DHT, AAHD, Linear };

// AHD is the default everywhere: an absent or unknown token resolves to it, so
// existing sidecars and the golden-image suite (ADR 0005) do not move.
inline constexpr DemosaicAlgorithm kDefaultDemosaic = DemosaicAlgorithm::AHD;

// libraw `imgdata.params.user_qual` integer for an algorithm (ADR 0036 table).
int librawUserQual(DemosaicAlgorithm algo);

// Stable persistence token ("AHD", "VNG", ...) written to arraw:DemosaicAlgorithm.
QString demosaicToken(DemosaicAlgorithm algo);

// Inverse of demosaicToken. An empty, unknown, or garbage token falls back
// silently to kDefaultDemosaic (the re-derive-don't-error contract of ADR 0021).
DemosaicAlgorithm demosaicFromToken(const QString& token);

// Whether the seven Bayer algorithms above actually apply to a sensor, given
// libraw's `imgdata.idata.filters` mosaic mask. False for X-Trans (9, which
// libraw reinterprets as Markesteijn) and for Foveon / already-demosaiced /
// linear / monochrome (0). On these the UI disables the control rather than
// offering Bayer labels that would not run (ADR 0036).
bool sensorSupportsDemosaicSelection(unsigned filters);
