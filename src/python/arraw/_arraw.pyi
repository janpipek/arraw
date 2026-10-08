"""Python bindings for the arraw RAW processing engine."""

from collections.abc import Callable, Sequence, Set
import enum
import os
import pathlib
from typing import Annotated, overload

import numpy
from numpy.typing import NDArray


class ImageSize:
    """Pixel dimensions of an image."""

    def __init__(self, width: int, height: int) -> None: ...

    @property
    def width(self) -> int: ...

    @property
    def height(self) -> int: ...

    def __repr__(self) -> str: ...

    def __eq__(self, arg: ImageSize, /) -> bool: ...

    def __hash__(self) -> int: ...

class ImageOrientation(enum.Enum):
    """Source orientation, the eight EXIF values."""

    NORMAL = 1

    MIRROR_HORIZONTAL = 2

    ROTATE_180 = 3

    MIRROR_VERTICAL = 4

    TRANSPOSE = 5

    ROTATE_90 = 6

    TRANSVERSE = 7

    ROTATE_270 = 8

class PixelFormat(enum.Enum):
    """Channel layout and sample type of a buffer."""

    RGB_U8 = 0

    RGBA_U8 = 1

    RGB_U16 = 2

    RGBA_U16 = 3

    RGB_F32 = 4

    RGBA_F32 = 5

class NamedEncoding(enum.Enum):
    """Colour encoding with fixed primaries and transfer."""

    LINEAR_REC2020 = 0

    SRGB = 1

    DISPLAY_P3 = 2

    ADOBE_RGB = 3

    REC2020_GAMMA22 = 4
    """
    Rec.2020 primaries, each channel sign(v) * |v|^(1/2.2): the curve input that sample() hands back; not an output encoding.
    """

class CameraNative:
    """A camera's native colour encoding; opaque in this version."""

    def __eq__(self, arg: CameraNative, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

class ImageMetadata:
    """What a photograph declares about itself."""

    @property
    def size(self) -> ImageSize: ...

    @property
    def orientation(self) -> ImageOrientation: ...

    @property
    def encoding(self) -> object:
        """NamedEncoding, or an opaque CameraNative."""

    def __eq__(self, arg: ImageMetadata, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

class ImageBuffer:
    """Decoded pixels; not constructible from Python."""

    @property
    def size(self) -> ImageSize: ...

    @property
    def format(self) -> PixelFormat: ...

    @property
    def encoding(self) -> object: ...

    @property
    def orientation(self) -> ImageOrientation: ...

    @property
    def pixel_scale(self) -> float:
        """
        Sensor pixels one pixel spans along each side: 1 for a full decode, 2 for a half-size one, doubled by each halving. Noise reduction divides its reach by it. Finite and above zero.
        """

    @pixel_scale.setter
    def pixel_scale(self, arg: float, /) -> None: ...

    @property
    def pixels(self) -> NDArray:
        """
        Writable NumPy view of shape (height, width, channels), without a copy; it keeps the buffer alive.
        """

    def __repr__(self) -> str: ...

def read_metadata(path: str | os.PathLike) -> ImageMetadata:
    """Read what a file declares about itself, without decoding its pixels."""

def load(path: str | os.PathLike, half_size: bool = False) -> ImageBuffer:
    """
    Decode an image file into a buffer. With half_size a RAW is decoded at half its width and height, without demosaicing, its pixel_scale is 2, and the buffer is that much smaller than read_metadata says; other files ignore it.
    """

def read_embedded_preview(path: str | os.PathLike, max_edge: int) -> ImageBuffer | None:
    """
    Read the preview a file embeds, upright, in sRGB and no longer than max_edge on its longer edge (0 keeps the largest at its own size). None if the file has no preview, or cannot be read, which is also a message on the 'arraw' logger.
    """

class URational:
    """
    An unsigned fraction, as EXIF stores an exposure time or an f-number: a numerator and a denominator.
    """

    def __init__(self, numerator: int | None = 0, denominator: int | None = 1) -> None: ...

    @property
    def numerator(self) -> int: ...

    @property
    def denominator(self) -> int: ...

    def __eq__(self, arg: URational, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> URational:
        """Return a copy with the given attributes replaced."""

    def value(self) -> float:
        """The quotient as a float, or NaN when the denominator is zero."""

class SRational:
    """A signed fraction, as EXIF stores an exposure bias."""

    def __init__(self, numerator: int | None = 0, denominator: int | None = 1) -> None: ...

    @property
    def numerator(self) -> int: ...

    @property
    def denominator(self) -> int: ...

    def __eq__(self, arg: SRational, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> SRational:
        """Return a copy with the given attributes replaced."""

    def value(self) -> float:
        """The quotient as a float, or NaN when the denominator is zero."""

class GpsPosition:
    """
    Where a photograph was taken: signed decimal degrees (negative south and west) and metres above sea level.
    """

    def __init__(self, *, latitude: float | None = 0.0, longitude: float | None = 0.0, altitude: float | None = None) -> None: ...

    @property
    def latitude(self) -> float: ...

    @property
    def longitude(self) -> float: ...

    @property
    def altitude(self) -> float | None: ...

    def __eq__(self, arg: GpsPosition, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> GpsPosition:
        """Return a copy with the given attributes replaced."""

class ExifInfo:
    """
    What a photograph's EXIF records about its capture; every field is None when absent.
    """

    def __init__(self, *, make: str | None = None, model: str | None = None, lens_model: str | None = None, date_time_original: str | None = None, offset_time_original: str | None = None, exposure_time: URational | None = None, f_number: URational | None = None, photographic_sensitivity: int | None = None, focal_length: URational | None = None, focal_length_in_35mm_film: int | None = None, exposure_bias_value: SRational | None = None, flash: int | None = None, gps: GpsPosition | None = None, artist: str | None = None, copyright: str | None = None) -> None: ...

    @property
    def make(self) -> str | None: ...

    @property
    def model(self) -> str | None: ...

    @property
    def lens_model(self) -> str | None: ...

    @property
    def date_time_original(self) -> str | None: ...

    @property
    def offset_time_original(self) -> str | None: ...

    @property
    def exposure_time(self) -> URational | None: ...

    @property
    def f_number(self) -> URational | None: ...

    @property
    def photographic_sensitivity(self) -> int | None: ...

    @property
    def focal_length(self) -> URational | None: ...

    @property
    def focal_length_in_35mm_film(self) -> int | None: ...

    @property
    def exposure_bias_value(self) -> SRational | None: ...

    @property
    def flash(self) -> int | None: ...

    @property
    def gps(self) -> GpsPosition | None: ...

    @property
    def artist(self) -> str | None: ...

    @property
    def copyright(self) -> str | None: ...

    def __eq__(self, arg: ExifInfo, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ExifInfo:
        """Return a copy with the given attributes replaced."""

def read_exif(path: str | os.PathLike) -> ExifInfo:
    """
    Read what a file records about its capture (EXIF). A file with no readable EXIF gives an ExifInfo with every field None, and a message on the 'arraw' logger; a file that does not exist raises.
    """

class WhiteBalanceMode(enum.Enum):
    """Where a photograph's white balance comes from."""

    AS_SHOT = 0

    CUSTOM = 1

class QuarterTurn(enum.Enum):
    """User rotation clockwise, in exact quarter-turns."""

    NONE = 0

    CLOCKWISE_90 = 1

    CLOCKWISE_180 = 2

    CLOCKWISE_270 = 3

class SettingGroup(enum.Enum):
    """Panel a setting belongs to."""

    COLOR = 0

    TONE = 1

    GEOMETRY = 2

    HSL = 3

    BLACK_AND_WHITE = 4

    TONE_CURVE = 5

    COLOR_GRADING = 6

    EFFECTS = 7

    DETAIL = 8

    PRESENCE = 9

class Applicability(enum.Enum):
    """Whether a setting means anything for every photograph."""

    ALWAYS = 0

    RAW_ONLY = 1

class SettingScope(enum.Enum):
    """Whether a setting is part of a look, or belongs to one photograph."""

    LOOK = 0

    PHOTO = 1

class GrainModel(enum.Enum):
    """Algorithm that draws the grain."""

    VALUE_NOISE = 0

class LuminanceNoiseFilter(enum.Enum):
    """Filter that smooths luminance noise."""

    BILATERAL = 0

class Stage(enum.Enum):
    """Pass boundary of the render pipeline."""

    DENOISE = 0

    POINTWISE = 1

    GEOMETRY = 2

    RESIZE = 3

    EFFECTS = 4

class ToneSettings:
    """Photographic tone adjustments."""

    def __init__(self, *, exposure: float | None = 0.0, contrast: float | None = 0.0, shadows: float | None = 0.0, highlights: float | None = 0.0, blacks: float | None = 0.0, whites: float | None = 0.0, filmic_highlights: float | None = 25.0) -> None: ...

    @property
    def exposure(self) -> float: ...

    @property
    def contrast(self) -> float: ...

    @property
    def shadows(self) -> float: ...

    @property
    def highlights(self) -> float: ...

    @property
    def blacks(self) -> float: ...

    @property
    def whites(self) -> float: ...

    @property
    def filmic_highlights(self) -> float: ...

    def __eq__(self, arg: ToneSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ToneSettings:
        """Return a copy with the given attributes replaced."""

class ColorSettings:
    """Photographic colour adjustments."""

    def __init__(self, *, white_balance: WhiteBalanceMode = WhiteBalanceMode.AS_SHOT, temperature: float | None | None = None, tint: float | None | None = None, saturation: float | None = 0.0, vibrance: float | None = 0.0) -> None: ...

    @property
    def white_balance(self) -> WhiteBalanceMode: ...

    @property
    def temperature(self) -> float | None: ...

    @property
    def tint(self) -> float | None: ...

    @property
    def saturation(self) -> float: ...

    @property
    def vibrance(self) -> float: ...

    def __eq__(self, arg: ColorSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ColorSettings:
        """Return a copy with the given attributes replaced."""

class HueBand:
    """Hue, saturation and luminance shifts of one band of hues."""

    def __init__(self, *, hue: float | None = 0.0, saturation: float | None = 0.0, luminance: float | None = 0.0) -> None: ...

    @property
    def hue(self) -> float: ...

    @property
    def saturation(self) -> float: ...

    @property
    def luminance(self) -> float: ...

    def __eq__(self, arg: HueBand, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> HueBand:
        """Return a copy with the given attributes replaced."""

class HslSettings:
    """Per-hue colour adjustments over eight bands."""

    def __init__(self, *, red: HueBand | None = None, orange: HueBand | None = None, yellow: HueBand | None = None, green: HueBand | None = None, aqua: HueBand | None = None, blue: HueBand | None = None, purple: HueBand | None = None, magenta: HueBand | None = None) -> None: ...

    @property
    def red(self) -> HueBand: ...

    @property
    def orange(self) -> HueBand: ...

    @property
    def yellow(self) -> HueBand: ...

    @property
    def green(self) -> HueBand: ...

    @property
    def aqua(self) -> HueBand: ...

    @property
    def blue(self) -> HueBand: ...

    @property
    def purple(self) -> HueBand: ...

    @property
    def magenta(self) -> HueBand: ...

    def __eq__(self, arg: HslSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> HslSettings:
        """Return a copy with the given attributes replaced."""

class BlackAndWhiteSettings:
    """Conversion to grey and the mix of hues it is made from."""

    def __init__(self, *, convert_to_grayscale: bool = False, red: float | None = 0.0, orange: float | None = 0.0, yellow: float | None = 0.0, green: float | None = 0.0, aqua: float | None = 0.0, blue: float | None = 0.0, purple: float | None = 0.0, magenta: float | None = 0.0) -> None: ...

    @property
    def convert_to_grayscale(self) -> bool: ...

    @property
    def red(self) -> float: ...

    @property
    def orange(self) -> float: ...

    @property
    def yellow(self) -> float: ...

    @property
    def green(self) -> float: ...

    @property
    def aqua(self) -> float: ...

    @property
    def blue(self) -> float: ...

    @property
    def purple(self) -> float: ...

    @property
    def magenta(self) -> float: ...

    def __eq__(self, arg: BlackAndWhiteSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> BlackAndWhiteSettings:
        """Return a copy with the given attributes replaced."""

class FreeCropAspect:
    """Unconstrained crop aspect."""

    def __init__(self) -> None: ...

    def __eq__(self, arg: FreeCropAspect, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> FreeCropAspect:
        """Return a copy with the given attributes replaced."""

class OriginalCropAspect:
    """Original image aspect after orientation and quarter-turns."""

    def __init__(self) -> None: ...

    def __eq__(self, arg: OriginalCropAspect, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> OriginalCropAspect:
        """Return a copy with the given attributes replaced."""

class CropRatio:
    """Fixed width-to-height crop ratio."""

    def __init__(self, width_over_height: float | None = 1.0) -> None: ...

    @property
    def width_over_height(self) -> float: ...

    def __eq__(self, arg: CropRatio, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> CropRatio:
        """Return a copy with the given attributes replaced."""

class UprightCropRect:
    """Crop edges normalised to the uncropped upright rectangle."""

    def __init__(self, left: float | None = 0.0, top: float | None = 0.0, right: float | None = 1.0, bottom: float | None = 1.0) -> None: ...

    @property
    def left(self) -> float: ...

    @property
    def top(self) -> float: ...

    @property
    def right(self) -> float: ...

    @property
    def bottom(self) -> float: ...

    def __eq__(self, arg: UprightCropRect, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> UprightCropRect:
        """Return a copy with the given attributes replaced."""

class CropSettings:
    """Framing and its remembered aspect constraint."""

    def __init__(self, *, rectangle: UprightCropRect | None = None, aspect: FreeCropAspect | OriginalCropAspect | CropRatio | None = None) -> None: ...

    @property
    def rectangle(self) -> UprightCropRect | None: ...

    @property
    def aspect(self) -> FreeCropAspect | OriginalCropAspect | CropRatio: ...

    def __eq__(self, arg: CropSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> CropSettings:
        """Return a copy with the given attributes replaced."""

class GeometrySettings:
    """Orientation, straightening and crop."""

    def __init__(self, *, rotation: QuarterTurn = QuarterTurn.NONE, flip_horizontal: bool = False, flip_vertical: bool = False, straighten: float | None = 0.0, crop: CropSettings | None = None) -> None: ...

    @property
    def rotation(self) -> QuarterTurn: ...

    @property
    def flip_horizontal(self) -> bool: ...

    @property
    def flip_vertical(self) -> bool: ...

    @property
    def straighten(self) -> float: ...

    @property
    def crop(self) -> CropSettings: ...

    def __eq__(self, arg: GeometrySettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> GeometrySettings:
        """Return a copy with the given attributes replaced."""

class ToneCurve:
    """
    A tone curve as 2 to 16 (x, y) control points from x = 0 to x = 1, x at least 0.01 apart, given in any order and sorted by x; the default is the identity.
    """

    def __init__(self, points: Sequence[tuple[float, float]] | None = None) -> None: ...

    @property
    def points(self) -> list[tuple[float, float]]: ...

    def __eq__(self, arg: ToneCurve, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ToneCurve:
        """Return a copy with the given attributes replaced."""

    @property
    def is_identity(self) -> bool:
        """Whether the curve is exactly the line from (0, 0) to (1, 1)."""

class ToneCurveSettings:
    """Tone curves on luminance and on the red, green and blue channels."""

    def __init__(self, *, luma: ToneCurve | None = None, red: ToneCurve | None = None, green: ToneCurve | None = None, blue: ToneCurve | None = None) -> None: ...

    @property
    def luma(self) -> ToneCurve: ...

    @property
    def red(self) -> ToneCurve: ...

    @property
    def green(self) -> ToneCurve: ...

    @property
    def blue(self) -> ToneCurve: ...

    def __eq__(self, arg: ToneCurveSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ToneCurveSettings:
        """Return a copy with the given attributes replaced."""

class GradeZone:
    """
    Tint of one tonal zone: a hue and how much of it.

    The hue is an Oklab hue angle in degrees, not Lightroom's: roughly 30 is red, 110 yellow, 140 green and 260 blue.
    """

    def __init__(self, *, hue: float | None = 0.0, saturation: float | None = 0.0) -> None: ...

    @property
    def hue(self) -> float: ...

    @property
    def saturation(self) -> float: ...

    def __eq__(self, arg: GradeZone, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> GradeZone:
        """Return a copy with the given attributes replaced."""

class ColorGradingSettings:
    """Three-zone toning of the shadows, midtones and highlights."""

    def __init__(self, *, shadows: GradeZone | None = None, midtones: GradeZone | None = None, highlights: GradeZone | None = None, balance: float | None = 0.0, blending: float | None = 50.0) -> None: ...

    @property
    def shadows(self) -> GradeZone: ...

    @property
    def midtones(self) -> GradeZone: ...

    @property
    def highlights(self) -> GradeZone: ...

    @property
    def balance(self) -> float: ...

    @property
    def blending(self) -> float: ...

    def __eq__(self, arg: ColorGradingSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ColorGradingSettings:
        """Return a copy with the given attributes replaced."""

class VignetteSettings:
    """
    Post-crop vignette: an elliptical falloff fitted to the cropped frame.

    Negative amounts darken the edges as an exposure change, positive ones lighten them toward white without passing it.
    """

    def __init__(self, *, amount: float | None = 0.0, midpoint: float | None = 50.0, feather: float | None = 50.0) -> None: ...

    @property
    def amount(self) -> float: ...

    @property
    def midpoint(self) -> float: ...

    @property
    def feather(self) -> float: ...

    def __eq__(self, arg: VignetteSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> VignetteSettings:
        """Return a copy with the given attributes replaced."""

class GrainSettings:
    """
    Film-like grain anchored to the cropped frame and to a seed.

    The seed is the photograph's own, not part of a look: 0 renders one fixed pattern, and choose_grain_seed gives grain an edit turns on a seed of its own.
    """

    def __init__(self, *, amount: float | None = 0.0, size: float | None = 50.0, roughness: float | None = 50.0, model: GrainModel = GrainModel.VALUE_NOISE, seed: int | None = 0) -> None: ...

    @property
    def amount(self) -> float: ...

    @property
    def size(self) -> float: ...

    @property
    def roughness(self) -> float: ...

    @property
    def model(self) -> GrainModel: ...

    @property
    def seed(self) -> int: ...

    def __eq__(self, arg: GrainSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> GrainSettings:
        """Return a copy with the given attributes replaced."""

class EffectsSettings:
    """Effects applied to the cropped frame after the resize."""

    def __init__(self, *, vignette: VignetteSettings | None = None, grain: GrainSettings | None = None) -> None: ...

    @property
    def vignette(self) -> VignetteSettings: ...

    @property
    def grain(self) -> GrainSettings: ...

    def __eq__(self, arg: EffectsSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> EffectsSettings:
        """Return a copy with the given attributes replaced."""

class PresenceSettings:
    """
    Texture, Clarity and Dehaze: local contrast after the tone controls, each -100 to 100.

    Texture acts on detail a few sensor pixels across, Clarity on the midtones' contrast at a hundredth of the long edge, Dehaze removes (or adds) a veil with some contrast and colour.
    """

    def __init__(self, *, texture: float | None = 0.0, clarity: float | None = 0.0, dehaze: float | None = 0.0) -> None: ...

    @property
    def texture(self) -> float: ...

    @property
    def clarity(self) -> float: ...

    @property
    def dehaze(self) -> float: ...

    def __eq__(self, arg: PresenceSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> PresenceSettings:
        """Return a copy with the given attributes replaced."""

class NoiseReductionSettings:
    """
    Luminance and colour noise reduction, run on the decoded photograph first.

    Radii are in sensor pixels; with both amounts at 0 nothing happens.
    """

    def __init__(self, *, luminance: float | None = 0.0, luminance_detail: float | None = 50.0, luminance_filter: LuminanceNoiseFilter = LuminanceNoiseFilter.BILATERAL, color: float | None = 0.0, color_smoothness: float | None = 50.0) -> None: ...

    @property
    def luminance(self) -> float: ...

    @property
    def luminance_detail(self) -> float: ...

    @property
    def luminance_filter(self) -> LuminanceNoiseFilter: ...

    @property
    def color(self) -> float: ...

    @property
    def color_smoothness(self) -> float: ...

    def __eq__(self, arg: NoiseReductionSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> NoiseReductionSettings:
        """Return a copy with the given attributes replaced."""

def choose_grain_seed(previous: GrainSettings, next: GrainSettings, entropy: Callable[[], int] | None = None) -> int:
    """
    Return the seed grain should carry after an edit from `previous` to `next`: a new one, never 0, when the edit turns grain on (amount from 0 to above 0) and `next` has none, else `next`'s seed, 0 included. `entropy` returns 32 random bits per call; None uses the operating system's. Store the result as the photograph's grain seed.
    """

class DevelopSettings:
    """Photographic settings of one photograph."""

    def __init__(self, *, color: ColorSettings | None = None, geometry: GeometrySettings | None = None, tone: ToneSettings | None = None, presence: PresenceSettings | None = None, hsl: HslSettings | None = None, black_and_white: BlackAndWhiteSettings | None = None, tone_curve: ToneCurveSettings | None = None, color_grading: ColorGradingSettings | None = None, effects: EffectsSettings | None = None, noise_reduction: NoiseReductionSettings | None = None) -> None: ...

    @property
    def color(self) -> ColorSettings: ...

    @property
    def geometry(self) -> GeometrySettings: ...

    @property
    def tone(self) -> ToneSettings: ...

    @property
    def presence(self) -> PresenceSettings: ...

    @property
    def hsl(self) -> HslSettings: ...

    @property
    def black_and_white(self) -> BlackAndWhiteSettings: ...

    @property
    def tone_curve(self) -> ToneCurveSettings: ...

    @property
    def color_grading(self) -> ColorGradingSettings: ...

    @property
    def effects(self) -> EffectsSettings: ...

    @property
    def noise_reduction(self) -> NoiseReductionSettings: ...

    def __eq__(self, arg: DevelopSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> DevelopSettings:
        """Return a copy with the given attributes replaced."""

    def to_json(self) -> str:
        """Write the settings as a JSON document."""

    @staticmethod
    def from_json(text: str, base: DevelopSettings | None = None) -> DevelopSettings:
        """
        Read a JSON document onto `base` (the defaults when None). Keys that are absent keep the base's value; problems with single settings are logged as warnings on the 'arraw' logger, and a document that cannot be read raises ValueError.
        """

    def with_(self, **kwargs: Any) -> DevelopSettings:
        """Return a copy with flat snake_case keywords applied, e.g. exposure=0.7. Applies no rules: each keyword is assigned as it is, so a temperature leaves the white balance as it was and a turn leaves the crop where it was. Photo.edited applies the rules of the editing frontends."""

class SettingDescriptor:
    """One row of the develop settings table."""

    @property
    def key(self) -> str: ...

    @property
    def name(self) -> str: ...

    @property
    def range(self) -> tuple[float, float] | None: ...

    @property
    def group(self) -> SettingGroup: ...

    @property
    def applies(self) -> Applicability: ...

    @property
    def affects(self) -> Stage: ...

    @property
    def scope(self) -> SettingScope: ...

    def __eq__(self, arg: SettingDescriptor, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

def setting_descriptors() -> list[SettingDescriptor]:
    """
    List the develop settings: key, snake_case name, range, group, applicability, stage, scope.
    """

class Severity(enum.Enum):
    """How serious a diagnostic is."""

    INFO = 0

    WARNING = 1

    ERROR = 2

class ResizeFilter(enum.Enum):
    """Resampling kernel for a develop to a size."""

    LANCZOS3 = 0
    """Windowed sinc of radius 3: sharp."""

    BILINEAR = 1
    """Tent kernel: soft, never rings."""

class ImageFileFormat(enum.Enum):
    """File format an image can be saved as."""

    JPEG = 0

    PNG = 1

    TIFF = 2

class ColorLabel(enum.Enum):
    """Colour a photograph is labelled with while culling."""

    RED = 0

    YELLOW = 1

    GREEN = 2

    BLUE = 3

    PURPLE = 4

class PhotoMarks:
    """Culling marks of a photograph: rating -1 (rejected) to 5, and a label."""

    def __init__(self, *, rating: int | None = 0, label: ColorLabel | None = None) -> None: ...

    @property
    def rating(self) -> int: ...

    @property
    def label(self) -> ColorLabel | None: ...

    def __eq__(self, arg: PhotoMarks, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> PhotoMarks:
        """Return a copy with the given attributes replaced."""

class ForeignNamespace:
    """
    A namespace other tools left properties in: its URI, the prefix as written, and how many top-level properties it holds.
    """

    def __init__(self, *, uri: str | None = None, prefix: str | None = None, properties: int | None = 0) -> None: ...

    @property
    def uri(self) -> str: ...

    @property
    def prefix(self) -> str: ...

    @property
    def properties(self) -> int: ...

    def __eq__(self, arg: ForeignNamespace, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ForeignNamespace:
        """Return a copy with the given attributes replaced."""

class DevelopState:
    """
    Everything that says how one photograph is developed: its global settings now, per-image edits later.
    """

    def __init__(self, *, settings: DevelopSettings | None = None) -> None: ...

    @property
    def settings(self) -> DevelopSettings: ...

    def __eq__(self, arg: DevelopState, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> DevelopState:
        """Return a copy with the given attributes replaced."""

class SidecarContents:
    """
    What an XMP sidecar holds, and which other tools wrote in it. `state` is None when the sidecar records no develop settings (marks only, or another tool's).
    """

    def __init__(self, *, state: DevelopState | None = None, marks: PhotoMarks | None = None, creator_tool: str | None = None, others: Sequence[ForeignNamespace] | None = None) -> None: ...

    @property
    def state(self) -> DevelopState | None: ...

    @property
    def marks(self) -> PhotoMarks: ...

    @property
    def creator_tool(self) -> str | None: ...

    @property
    def others(self) -> list[ForeignNamespace]: ...

    def __eq__(self, arg: SidecarContents, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> SidecarContents:
        """Return a copy with the given attributes replaced."""

@overload
def default_state(metadata: ImageMetadata) -> DevelopState:
    """
    The state a photograph of this kind starts from: colour noise reduction 25 for a RAW, the neutral DevelopState() for anything else. What open() gives a photograph with no sidecar.
    """

@overload
def default_state(buffer: ImageBuffer) -> DevelopState:
    """The state a decoded buffer's kind starts from, as for its metadata."""

def turned(metadata: ImageMetadata, state: DevelopState, clockwise: bool) -> DevelopState:
    """
    Turn the photograph by a quarter as it appears on screen, carrying the crop.
    """

def flipped(metadata: ImageMetadata, state: DevelopState, horizontal: bool) -> DevelopState:
    """Mirror the photograph as it appears on screen, carrying the crop."""

def with_aspect(metadata: ImageMetadata, state: DevelopState, aspect: FreeCropAspect | OriginalCropAspect | CropRatio) -> DevelopState:
    """
    Set the crop aspect, fitting the crop to a ratio. ValueError without the photograph's size.
    """

def with_locked_aspect(metadata: ImageMetadata, state: DevelopState) -> DevelopState:
    """Lock the aspect at the crop's present ratio."""

def with_swapped_orientation(metadata: ImageMetadata, state: DevelopState) -> DevelopState:
    """Swap portrait and landscape."""

def with_crop_reset(metadata: ImageMetadata, state: DevelopState) -> DevelopState:
    """Return to automatic framing, keeping the aspect constraint."""

def displayed_straighten(state: DevelopState) -> float:
    """
    The straighten as it appears on screen: degrees, clockwise positive. Needs no metadata.
    """

def with_displayed_straighten(metadata: ImageMetadata, state: DevelopState, displayed: float) -> DevelopState:
    """
    Straighten to an angle as it appears on screen (clockwise positive), shrinking the crop as the `straighten` setting does.
    """

def xmp_namespace_owner(uri: str) -> str | None:
    """
    Name the tool or standard behind an XMP namespace URI, or None when unknown.
    """

class Photo:
    """One photograph as a document: a file and how it is developed."""

    @property
    def path(self) -> pathlib.Path: ...

    @property
    def metadata(self) -> ImageMetadata: ...

    @property
    def state(self) -> DevelopState: ...

    @property
    def marks(self) -> PhotoMarks: ...

    def edited(self, **kwargs) -> Photo:
        """
        Return a photograph with flat snake_case keywords set, e.g. exposure=0.7, by the rules the app and the command line apply: a temperature makes the white balance Custom, grain turned on gets a seed, a turn carries the crop, a straighten shrinks it, a crop rectangle frees the aspect. Keywords are applied in the order given, so crop_aspect then crop_rectangle differs from the reverse. Raises TypeError for an unknown keyword or a wrong type, and ValueError for a value the setting refuses or a geometry that does not fit the photograph's size.
        """

    def load(self) -> ImageBuffer:
        """Decode the photograph's file into a buffer."""

    def __eq__(self, arg: Photo, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

    def with_(
        self,
        state: DevelopState | None = None,
        *,
        marks: PhotoMarks | None = None,
        rating: int = ...,
        label: ColorLabel | None = ...,
        **kwargs: Any,
    ) -> Photo:
        """
        Return a photograph with `state` (and `marks`) replacing the current ones wholesale, then flat snake_case keywords applied, e.g. exposure=0.7, which edit the settings of the state. `rating` and `label` change the marks instead (label=None clears it). Applies no rules: each keyword is assigned as it is, so a turn leaves the crop where it was. `edited` applies the rules of the editing frontends.
        """

def open(path: str | os.PathLike, *, sidecar: bool = True) -> Photo:
    """
    Open a photograph; reads its metadata, not its pixels. Its XMP sidecar supplies the state and marks unless sidecar=False. Without one, or with one that records no settings, the state is default_state(metadata). A sidecar that cannot be read is logged as an error on the 'arraw' logger, not raised, and the defaults are used.
    """

def sidecar_path(path: str | os.PathLike) -> pathlib.Path:
    """Name the XMP sidecar of a photograph; nothing is created."""

def read_sidecar(path: str | os.PathLike) -> SidecarContents | None:
    """Read the sidecar of a photograph, or None when it has none."""

def write_sidecar(photo: Photo) -> None:
    """
    Write a photograph's state and marks into its sidecar, keeping the rest.
    """

def write_sidecar_marks(path: str | os.PathLike, marks: PhotoMarks) -> None:
    """
    Write only the marks of a photograph into its sidecar, keeping its settings and the rest; a sidecar is created when there is none.
    """

@overload
def develop(source: ImageBuffer, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer:
    """
    Develop a decoded buffer on the CPU; with no state, the defaults of its kind (default_state: colour noise reduction for a RAW, nothing for anything else). `size` renders the cropped result smaller: an int is the long edge, a (width, height) tuple a box to fit inside, a float a scale factor. Sizes only shrink unless `allow_upscale`.
    """

@overload
def develop(source: Photo, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer:
    """
    Decode a photograph and develop it with its own state unless `state` is given; `size`, `filter` and `allow_upscale` are as for a decoded buffer.
    """

class Tap(enum.Enum):
    """Named position inside the pointwise chain that sample() stops at."""

    CURVE_INPUT = 0
    """
    What the tone curves take in: after white balance, exposure and Basic Tone; handed back in NamedEncoding.REC2020_GAMMA22.
    """

@overload
def sample(source: ImageBuffer, tap: Tap, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer:
    """
    Develop a decoded buffer on the CPU with the chain stopped at `tap`, to measure it: the same frame and size as develop() with the same arguments, in the tap's encoding.
    """

@overload
def sample(source: Photo, tap: Tap, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False) -> ImageBuffer:
    """
    Decode a photograph and sample it at `tap` with its own state unless `state` is given.
    """

CURVE_HISTOGRAM_BINS: int = 256

class CurveHistogram:
    """
    Pixel counts of the curve input over the perceptual coordinate, CURVE_HISTOGRAM_BINS bins from 0 to 1 per channel; not constructible from Python.
    """

    @property
    def luma(self) -> Annotated[NDArray[numpy.uint64], dict(shape=(256), writable=False)]:
        """
        Read-only uint64 array of the luminance counts, as the luma curve reads it.
        """

    @property
    def red(self) -> Annotated[NDArray[numpy.uint64], dict(shape=(256), writable=False)]:
        """Read-only uint64 array of the red counts."""

    @property
    def green(self) -> Annotated[NDArray[numpy.uint64], dict(shape=(256), writable=False)]:
        """Read-only uint64 array of the green counts."""

    @property
    def blue(self) -> Annotated[NDArray[numpy.uint64], dict(shape=(256), writable=False)]:
        """Read-only uint64 array of the blue counts."""

    @property
    def pixels(self) -> int:
        """Number of pixels counted; fully transparent ones are not."""

    def __eq__(self, arg: CurveHistogram, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

@overload
def curve_histogram(image: ImageBuffer) -> CurveHistogram:
    """
    Count a sample taken at Tap.CURVE_INPUT (NamedEncoding.REC2020_GAMMA22) into a CurveHistogram.
    """

@overload
def curve_histogram(source: ImageBuffer, state: DevelopState, *, size: int | tuple[int, int] | float | None = 1024, allow_upscale: bool = False) -> CurveHistogram:
    """
    Sample a decoded buffer at Tap.CURVE_INPUT and count it. `size` is as for develop() and defaults to a 1024-pixel long edge; None counts the full cropped resolution. The resize is always bilinear, so no ringing reaches the end bins.
    """

@overload
def curve_histogram(source: Photo, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = 1024, allow_upscale: bool = False) -> CurveHistogram:
    """
    Decode a photograph, sample it at Tap.CURVE_INPUT with its own state unless `state` is given, and count it; `size` and the resize as for a decoded buffer.
    """

def resolved_size(size: int | tuple[int, int] | float, cropped: ImageSize, *, allow_upscale: bool = False) -> ImageSize:
    """
    Resolve a develop `size` against the size after the crop, as develop does.
    """

class MetadataSelection:
    """
    Groups of metadata an export carries from its source photograph: capture (camera, lens, exposure, time), location (GPS) and descriptive (rating, label, title, caption, keywords, creator, rights).
    """

    def __init__(self, *, capture: bool = True, location: bool = False, descriptive: bool = True) -> None: ...

    @property
    def capture(self) -> bool: ...

    @property
    def location(self) -> bool: ...

    @property
    def descriptive(self) -> bool: ...

    def __eq__(self, arg: MetadataSelection, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> MetadataSelection:
        """Return a copy with the given attributes replaced."""

def save(image: ImageBuffer, path: str | os.PathLike, *, format: ImageFileFormat | None = None, encoding: NamedEncoding = NamedEncoding.SRGB, bit_depth: int = 8, quality: int = 90, embed_profile: bool = True, sharpening: int = 0, metadata_from: Photo | None = None, metadata: MetadataSelection = ...) -> None:
    """
    Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given. `sharpening` (0-100, default 0 = off) applies an unsharp mask to the final pixels. With `metadata_from` (the photograph the pixels came from) the groups of `metadata` are copied from its file and sidecar, and its marks written as rating and label; without it nothing is written. A source or sidecar that cannot be read is logged as a warning on the 'arraw' logger and its metadata left out; the file is still written.
    """

class Shot:
    """
    One capture: the file that is developed (the RAW when there is one) and the standard images of the same capture, which a RAW+JPEG camera writes beside it.
    """

    def __init__(self, *, primary: str | os.PathLike | None = None, companions: Sequence[str | os.PathLike] | None = None) -> None: ...

    @property
    def primary(self) -> pathlib.Path: ...

    @property
    def companions(self) -> list[pathlib.Path]: ...

    def __eq__(self, arg: Shot, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> Shot:
        """Return a copy with the given attributes replaced."""

    @property
    def format_label(self) -> str:
        """
        The formats of the shot, each once, joined by '+': 'ARW', 'JPEG', 'ARW+JPEG'.
        """

class MarksFilter:
    """
    Which culling marks a photograph must have: at least min_rating stars (which excludes rejects and unrated ones), or rejects_only (not both), and any one of labels (empty for any). The two dimensions combine by AND.
    """

    def __init__(self, *, min_rating: int | None = 0, rejects_only: bool = False, labels: Set[ColorLabel] | None = None) -> None: ...

    @property
    def min_rating(self) -> int: ...

    @property
    def rejects_only(self) -> bool: ...

    @property
    def labels(self) -> set[ColorLabel]: ...

    def __eq__(self, arg: MarksFilter, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> MarksFilter:
        """Return a copy with the given attributes replaced."""

    @property
    def is_active(self) -> bool:
        """Whether the filter narrows anything."""

    def matches(self, marks: PhotoMarks) -> bool:
        """
        Whether a photograph's marks pass the filter. Raises ValueError for a filter that wants rejects_only and a min_rating, or a min_rating outside 0 to 5.
        """

def group_shots(paths: Sequence[str | os.PathLike]) -> list[Shot]:
    """
    Group files into shots, without touching the filesystem: same folder and same stem (case-insensitive), the RAW as primary and the JPEG, PNG and TIFF files as its companions. Files without a RAW partner, and every file of a stem two RAWs share, stand alone; unsupported files and sidecars are dropped. In natural order of the primary's name (IMG_2 before IMG_10).
    """

def list_shots(folder: str | os.PathLike) -> list[Shot]:
    """
    List the shots of a folder (not its subfolders, not hidden files), in natural order. Raises if the folder cannot be read.
    """

def is_supported_image(path: str | os.PathLike) -> bool:
    """Whether a path names a photograph arraw opens, by its extension alone."""

SUPPORTED_EXTENSIONS: tuple = ...

class EditOrigin(enum.Enum):
    """Kind of change that made a history step, beyond what its states show."""

    OPENED = 0

    EDIT = 1

    PASTE = 2

    PRESET = 3

    RESET = 4

    CROP = 5

class HistoryStep:
    """One entry of an edit history: the state it left, and why."""

    @property
    def state(self) -> DevelopState:
        """Whole develop state after the step."""

    @property
    def origin(self) -> EditOrigin:
        """Kind of change the step was."""

    @property
    def detail(self) -> str:
        """What the origin applies to, such as a preset's name; empty otherwise."""

    def __eq__(self, arg: HistoryStep, /) -> bool: ...

    def __repr__(self) -> str: ...

class ChangeDescription:
    """Settings two develop states differ in, for naming a step."""

    @property
    def keys(self) -> list[str]:
        """
        Descriptor keys (camelCase, as JSON and the sidecar spell them) whose values differ, in table
        order; setting_descriptors() maps them to the Python names.
        """

    @property
    def group(self) -> SettingGroup | None:
        """
        Group every key belongs to; None when there are no keys or they span groups.
        """

    def __eq__(self, arg: ChangeDescription, /) -> bool: ...

    def __repr__(self) -> str: ...

def describe_change(before: DevelopState, after: DevelopState) -> ChangeDescription:
    """List the settings two develop states differ in."""

class EditScope:
    """Context manager of one edit; made by EditSession.edit()."""

    def __enter__(self) -> object: ...

    def __exit__(self, type: object | None, value: object | None, traceback: object | None) -> bool: ...

class EditSession:
    """
    One photograph being edited: the document as it stands, and how it got there.
    """

    def __init__(self, photo: Photo) -> None: ...

    @property
    def photo(self) -> Photo:
        """Current photograph, including an edit in progress."""

    def begin(self) -> None:
        """Open an edit; one already open is committed first."""

    def update(self, state: DevelopState) -> None:
        """Change the state provisionally, inside the open edit."""

    def commit(self, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """
        Close the open edit as one history step; an edit that ends where it began leaves none.
        """

    def cancel(self) -> None:
        """Close the open edit, restoring its starting state."""

    @property
    def editing(self) -> bool:
        """Whether an edit is open."""

    def set_state(self, state: DevelopState, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """Replace the develop state as one history step."""

    @property
    def history(self) -> list[HistoryStep]:
        """Steps taken so far, the opening state first; a new list on every access."""

    @property
    def position(self) -> int:
        """Index in history of the step the document is at."""

    def go_to(self, index: int) -> None:
        """
        Move to a step, keeping every step. An open edit is committed first, and index is taken after that.
        """

    @property
    def can_undo(self) -> bool: ...

    @property
    def can_redo(self) -> bool: ...

    def undo(self) -> None:
        """Take back the latest history step."""

    def redo(self) -> None:
        """Bring back the step undo took back."""

    @property
    def saved(self) -> Photo:
        """Photograph as its sidecar holds it."""

    @property
    def has_unsaved_changes(self) -> bool: ...

    def save(self) -> None:
        """Write the photograph to its sidecar and make that the saved state."""

    def set_marks(self, marks: PhotoMarks) -> None:
        """Set the marks, writing them to the sidecar at once."""

    def discard_changes(self) -> None:
        """Return to the saved state, dropping history and any open edit."""

    def edit(self, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> EditScope:
        """
        Context manager for one edit: commits on a normal exit, cancels if the block raises. Entering raises RuntimeError while an edit is open; leaving does nothing if the block closed the edit itself.
        """

__version__: str
