"""Python bindings for the arraw RAW processing engine."""

from collections.abc import Callable, Sequence, Set
import enum
import os
import pathlib
from typing import Annotated, Any, overload

import numpy
from numpy.typing import NDArray


def lensfun_available() -> bool:
    """
    Whether this build can read a lens database at all (it was built with lensfun).
    """

def lensfun_version() -> str:
    """
    The lensfun this build was compiled against, such as '0.3.4'; empty without lensfun.
    """

class LensOrigin(enum.Enum):
    """
    How the lens recorded in a photograph's lens settings was chosen; str() is the token the sidecar uses.
    """

    def __str__(self) -> str: ...

    MATCHED = 0
    """Found from the file's EXIF."""

    CHOSEN = 1
    """Picked by the photographer."""

class LensProblem(enum.Enum):
    """Why a lens could not be found or loaded."""

    LENS_UNIDENTIFIED = 0

    LENS_FOCAL_UNKNOWN = 1

    CAMERA_NOT_IN_DATABASE = 2

    LENS_NOT_IN_DATABASE = 3

    LENS_AMBIGUOUS = 4

    LENS_PROFILE_MISSING = 5

    LENS_DATABASE_CHANGED = 6

    LENS_DATABASE_MISSING = 7

    LENSFUN_UNAVAILABLE = 8

class CropSource(enum.Enum):
    """Where the crop factor of a lens match came from."""

    CAMERA = 0
    """The camera listed in the database."""

    FOCAL_RATIO = 1
    """The 35 mm focal length over the focal length."""

    LENS = 2
    """The lens's own, when nothing else says."""

class LensIdentity:
    """A lens by name, exactly as lensfun spells its maker and model."""

    def __init__(self, maker: str | None = None, model: str | None = None) -> None: ...

    @property
    def maker(self) -> str: ...

    @property
    def model(self) -> str: ...

    def __eq__(self, arg: LensIdentity, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LensIdentity:
        """Return a copy with the given attributes replaced."""

class RecordedLens:
    """
    The lens a photograph's corrections were found for, by name only: lensfun's maker and model, the crop factor of the image as the match used it, how the lens was chosen, and the stamp of the database at the time. No calibration is recorded.
    """

    def __init__(self, *, maker: str | None = None, model: str | None = None, crop_factor: float | None = 1.0, origin: LensOrigin = LensOrigin.MATCHED, database: str | None = None) -> None: ...

    @property
    def maker(self) -> str: ...

    @property
    def model(self) -> str: ...

    @property
    def crop_factor(self) -> float: ...

    @property
    def origin(self) -> LensOrigin: ...

    @property
    def database(self) -> str: ...

    def __eq__(self, arg: RecordedLens, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> RecordedLens:
        """Return a copy with the given attributes replaced."""

class LensSettings:
    """
    Which lens corrections a photograph asks for (all off by default), and the lens recorded for them, None until one is first turned on. They apply to a RAW only.
    """

    def __init__(self, *, distortion: bool = False, vignetting_amount: float | None = 0.0, lateral_ca: bool = False, lens: RecordedLens | None = None) -> None: ...

    @property
    def distortion(self) -> bool: ...

    @property
    def vignetting_amount(self) -> float: ...

    @property
    def lateral_ca(self) -> bool: ...

    @property
    def lens(self) -> RecordedLens | None: ...

    def __eq__(self, arg: LensSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LensSettings:
        """Return a copy with the given attributes replaced."""

    @property
    def any(self) -> bool:
        """Whether any correction is asked for."""

class LensFacts:
    """
    What a RAW says about its camera and lens, for a profile to be found by; empty for any other file.
    """

    def __init__(self, *, camera_maker: str | None = None, camera_model: str | None = None, lens_names: Sequence[str] | None = None, min_focal: float | None = None, max_focal: float | None = None, max_aperture: float | None = None, focal_length: float | None = None, aperture: float | None = None, focus_distance: float | None = None, focal_length_35: float | None = None) -> None: ...

    @property
    def camera_maker(self) -> str: ...

    @property
    def camera_model(self) -> str: ...

    @property
    def lens_names(self) -> list[str]: ...

    @property
    def min_focal(self) -> float | None: ...

    @property
    def max_focal(self) -> float | None: ...

    @property
    def max_aperture(self) -> float | None: ...

    @property
    def focal_length(self) -> float | None: ...

    @property
    def aperture(self) -> float | None: ...

    @property
    def focus_distance(self) -> float | None: ...

    @property
    def focal_length_35(self) -> float | None: ...

    def __eq__(self, arg: LensFacts, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LensFacts:
        """Return a copy with the given attributes replaced."""

class CameraEntry:
    """A camera as the lens database lists it."""

    def __init__(self, *, maker: str | None = None, model: str | None = None, mount: str | None = None, crop_factor: float | None = 1.0) -> None: ...

    @property
    def maker(self) -> str: ...

    @property
    def model(self) -> str: ...

    @property
    def mount(self) -> str: ...

    @property
    def crop_factor(self) -> float: ...

    def __eq__(self, arg: CameraEntry, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> CameraEntry:
        """Return a copy with the given attributes replaced."""

class LensEntry:
    """A lens as the lens database lists it."""

    def __init__(self, *, maker: str | None = None, model: str | None = None, mounts: Sequence[str] | None = None, crop_factor: float | None = 1.0, min_focal: float | None = 0.0, max_focal: float | None = 0.0, widest_aperture: float | None = 0.0, score: int | None = 0) -> None: ...

    @property
    def maker(self) -> str: ...

    @property
    def model(self) -> str: ...

    @property
    def mounts(self) -> list[str]: ...

    @property
    def crop_factor(self) -> float: ...

    @property
    def min_focal(self) -> float: ...

    @property
    def max_focal(self) -> float: ...

    @property
    def widest_aperture(self) -> float: ...

    @property
    def score(self) -> int: ...

    def __eq__(self, arg: LensEntry, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LensEntry:
        """Return a copy with the given attributes replaced."""

class LensMatch:
    """
    A lens found for a photograph, and what it was found with; not constructible from Python.
    """

    @property
    def lens(self) -> LensIdentity:
        """The lens found."""

    @property
    def name(self) -> str:
        """The name from the photograph that found it."""

    @property
    def score(self) -> int:
        """lensfun's score for that name."""

    @property
    def exact_name(self) -> bool:
        """Whether the name is the lens's whole name, not merely like it."""

    @property
    def camera_listed(self) -> bool:
        """Whether the camera is in the database; informational when not."""

    @property
    def crop_factor(self) -> float:
        """Crop factor of the image, as the profile is applied with."""

    @property
    def crop_source(self) -> CropSource:
        """Where the crop factor came from."""

    @property
    def focal_length(self) -> float:
        """Focal length to resolve the profile at, in millimetres."""

    @property
    def focal_length_read(self) -> bool:
        """Whether the file recorded the focal length; a prime's own is assumed."""

    def __eq__(self, arg: LensMatch, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

class LensProvenance:
    """
    Where one correction's numbers came from: the lens, the conditions it was resolved for, and whether the file recorded each or it was assumed.
    """

    def __init__(self, *, lens: LensIdentity | None = None, crop_factor: float | None = 1.0, focal_length: float | None = 0.0, focal_length_read: bool = False, aperture: float | None = 0.0, aperture_read: bool = False, distance: float | None = 0.0, distance_read: bool = False, database: str | None = None) -> None: ...

    @property
    def lens(self) -> LensIdentity: ...

    @property
    def crop_factor(self) -> float: ...

    @property
    def focal_length(self) -> float: ...

    @property
    def focal_length_read(self) -> bool: ...

    @property
    def aperture(self) -> float: ...

    @property
    def aperture_read(self) -> bool: ...

    @property
    def distance(self) -> float: ...

    @property
    def distance_read(self) -> bool: ...

    @property
    def database(self) -> str: ...

    def __eq__(self, arg: LensProvenance, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LensProvenance:
        """Return a copy with the given attributes replaced."""

class LensCorrection:
    """
    A lens's corrections resolved into radial tables, independent of resolution and of where the profile came from; not constructible from Python. The radius is 0 at the optical centre and 1 at the farthest corner, and each table holds the same number of values evenly spaced over it; an empty table means that correction is absent.
    """

    @property
    def active(self) -> bool:
        """Whether any correction is present, so whether the render has work."""

    @property
    def distortion(self) -> tuple:
        """
        Radial scale from a point of the corrected image to the sensor image, before the fill scale; empty for none.
        """

    @property
    def red_scale(self) -> tuple:
        """
        Scale of red's position against green's, by sensor radius; empty for none.
        """

    @property
    def blue_scale(self) -> tuple:
        """
        Scale of blue's position against green's, by sensor radius; empty for none.
        """

    @property
    def vignetting(self) -> tuple:
        """Gain that restores the light, by sensor radius; empty for none."""

    @property
    def center_u(self) -> float:
        """Optical centre across, as a fraction of the width."""

    @property
    def center_v(self) -> float:
        """Optical centre down, as a fraction of the height."""

    @property
    def aspect(self) -> float:
        """Width over height of the frames the tables were sampled for."""

    @property
    def fill(self) -> float:
        """
        Zoom that keeps every sample of the corrected frame inside the sensor image.
        """

    @property
    def distortion_from(self) -> LensProvenance | None:
        """Provenance of the distortion, or None."""

    @property
    def lateral_ca_from(self) -> LensProvenance | None:
        """Provenance of the lateral chromatic aberration, or None."""

    @property
    def vignetting_from(self) -> LensProvenance | None:
        """Provenance of the vignetting, or None."""

    def __eq__(self, arg: LensCorrection, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

class LensDatabase:
    """
    The lens and camera database, loaded once and read-only afterwards. One that could not be loaded is an object like any other, which finds nothing and says why in `problem`.
    """

    @staticmethod
    def system() -> LensDatabase:
        """
        The process's database, loaded on the first call (slow): beside the executable, else lensfun's own directories, the user's update replacing either when newer.
        """

    @staticmethod
    def from_directory(directory: str | os.PathLike) -> LensDatabase:
        """
        Load the database of one directory of lensfun XML files, and optionally a timestamp.txt naming its age. Never raises: check `loaded` and `problem`.
        """

    @property
    def loaded(self) -> bool:
        """Whether any database was loaded."""

    @property
    def problem(self) -> LensProblem | None:
        """Why nothing was loaded, or None."""

    @property
    def directory(self) -> pathlib.Path:
        """The directory it came from."""

    @property
    def stamp(self) -> str:
        """
        Its age, as its newest timestamp: a UTC time; empty when nothing loaded.
        """

    @property
    def camera_count(self) -> int: ...

    @property
    def lens_count(self) -> int: ...

    def find_camera(self, maker: str, model: str) -> CameraEntry | None:
        """
        Find a camera from its EXIF Make and Model, or None when it is not listed.
        """

    def find_lenses(self, name: str, *, mount: str | None = None, crop_factor: float | None = None) -> list[LensEntry]:
        """
        The lenses a name as a photograph records it could mean, best first by lensfun's score, within the mount and crop factor given.
        """

    def lens(self, maker: str, model: str, crop_factor: float | None = None) -> LensEntry | None:
        """
        Look a lens up by its exact lensfun name, or None. A name the database lists more than once is answered by the calibration for `crop_factor`.
        """

    def lenses(self, search: str = '', mount: str | None = None) -> list[LensEntry]:
        """
        List lenses whose maker and model hold every word of `search` (any order and case), ordered by maker and model, within a mount if one is given.
        """

    def mount_of(self, maker: str, model: str) -> str | None:
        """The mount of a camera, or None when it is not listed or has none."""

    def __repr__(self) -> str: ...

class Demosaic(enum.Enum):
    """
    Algorithm that turns a RAW's sensor mosaic into colour; str() is the token the sidecar and the command line use.
    """

    def __str__(self) -> str: ...

    LINEAR = 0

    VNG = 1

    PPG = 2

    AHD = 3

    DCB = 4

    DHT = 5

    AAHD = 6

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

    @property
    def lens(self) -> LensFacts:
        """
        What the file says about its camera and lens, for a profile to be found by; empty for anything but a RAW.
        """

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

def load(path: str | os.PathLike, half_size: bool = False, demosaic: Demosaic = Demosaic.AHD) -> ImageBuffer:
    """
    Decode an image file into a buffer. With half_size a RAW is decoded at half its width and height, without demosaicing, its pixel_scale is 2, and the buffer is that much smaller than read_metadata says; other files ignore it. demosaic is the algorithm of a full-size decode of a mosaiced RAW; a half-size decode and other files ignore it.
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

    LENS = 1

    POINTWISE = 2

    GEOMETRY = 3

    RESIZE = 4

    EFFECTS = 5

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

class LinearMask:
    """
    A graduated fade between two points of the corrected frame, each (u, v) normalised to the frame (0 to 1 across it; handles may lie outside, from -2 to 3). The weight is 1 at `from_`, 0 at `to`, and smooth between; it is constant along lines perpendicular to the one between them, measured in long-edge units so that a circle is a circle.
    """

    def __init__(self, from_: tuple[float, float] | None = None, to: tuple[float, float] | None = None) -> None: ...

    @property
    def to(self) -> tuple[float, float]: ...

    def __eq__(self, arg: LinearMask, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LinearMask:
        """Return a copy with the given attributes replaced."""

    @property
    def from_(self) -> tuple[float, float]:
        """Where the weight is 1: a (u, v) point normalised to the corrected frame."""

class RadialMask:
    """
    An oval with a feathered edge. `centre` is (u, v) normalised to the frame; the radii are in long-edge units (the longer side of the frame is 1); `angle` turns the x radius towards +y in degrees (clockwise on screen); `feather` is the width of the soft edge, 0 (hard, one pixel) to 1 (from the centre).
    """

    def __init__(self, centre: tuple[float, float] | None = None, radius_x: float | None = 0.25, radius_y: float | None = 0.25, angle: float | None = 0.0, feather: float | None = 0.5) -> None: ...

    @property
    def centre(self) -> tuple[float, float]: ...

    @property
    def radius_x(self) -> float: ...

    @property
    def radius_y(self) -> float: ...

    @property
    def angle(self) -> float: ...

    @property
    def feather(self) -> float: ...

    def __eq__(self, arg: RadialMask, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> RadialMask:
        """Return a copy with the given attributes replaced."""

class Stroke:
    """
    One brush stroke: a dab `radius` in long-edge units (0.0005 to 1), the `hardness` of its core (0 to 1), the `flow` of one dab (0 to 1), whether it `erase`s coverage, and the `points` of its path, (u, v) positions normalised to the frame, as a tuple. The numbers are kept as given and clamped when the stroke is made into a BrushMask or appended to one.
    """

    def __init__(self, radius: float = 0.02, hardness: float = 0.5, flow: float = 1.0, erase: bool = False, points: Sequence[tuple[float, float]] = ()) -> None: ...

    @property
    def radius(self) -> float: ...

    @property
    def hardness(self) -> float: ...

    @property
    def flow(self) -> float: ...

    @property
    def erase(self) -> bool: ...

    @property
    def points(self) -> tuple[tuple[float, float], ...]: ...

    def __eq__(self, arg: Stroke, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> Stroke:
        """Return a copy with the given attributes replaced."""

class BrushMask:
    """
    A stencil painted on the photograph: the `strokes`, in painting order, in the sensor frame. Making one clamps each stroke's numbers as adding a mask does, then builds the list; ValueError for a number that is not finite, a stroke without points, or a cap or budget passed (at most 2000 strokes, 100000 points, a swept area of 4 and 2 million dabs). `rasteriser` is the version of the rules that turn the strokes into coverage.
    """

    def __init__(self, strokes: Sequence[Stroke] = ()) -> None: ...

    @property
    def strokes(self) -> tuple[Stroke, ...]: ...

    @property
    def rasteriser(self) -> int: ...

    def __eq__(self, arg: BrushMask, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

class LocalDeltas:
    """
    What a local adjustment adds to the global controls where its mask has full weight, in the units of the global setting (stops for exposure, -100 to 100 for the rest; relative_temperature and relative_tint have no global counterpart). 0 changes nothing.
    """

    def __init__(self, *, relative_temperature: float | None = 0.0, relative_tint: float | None = 0.0, exposure: float | None = 0.0, contrast: float | None = 0.0, highlights: float | None = 0.0, shadows: float | None = 0.0, whites: float | None = 0.0, blacks: float | None = 0.0, texture: float | None = 0.0, clarity: float | None = 0.0, dehaze: float | None = 0.0, saturation: float | None = 0.0, vibrance: float | None = 0.0) -> None: ...

    @property
    def relative_temperature(self) -> float: ...

    @property
    def relative_tint(self) -> float: ...

    @property
    def exposure(self) -> float: ...

    @property
    def contrast(self) -> float: ...

    @property
    def highlights(self) -> float: ...

    @property
    def shadows(self) -> float: ...

    @property
    def whites(self) -> float: ...

    @property
    def blacks(self) -> float: ...

    @property
    def texture(self) -> float: ...

    @property
    def clarity(self) -> float: ...

    @property
    def dehaze(self) -> float: ...

    @property
    def saturation(self) -> float: ...

    @property
    def vibrance(self) -> float: ...

    def __eq__(self, arg: LocalDeltas, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LocalDeltas:
        """Return a copy with the given attributes replaced."""

class LocalAdjustment:
    """
    One masked adjustment: a LinearMask, RadialMask or BrushMask and the LocalDeltas added where it applies, scaled by `opacity` (0 to 1) and, with `invert`, applied outside the mask instead. `id` is the photograph's own identity for it, unique in its state and never reused; a value built here carries 0 and gets its id when added with Photo.add_*_mask. Adjustments are summed in list order; up to 16 per photograph.
    """

    def __init__(self, *, id: int | None = None, name: str | None = None, enabled: bool = True, opacity: float | None = 1.0, invert: bool = False, shape: LinearMask | RadialMask | BrushMask | None = None, deltas: LocalDeltas | None = None) -> None: ...

    @property
    def id(self) -> int: ...

    @property
    def name(self) -> str: ...

    @property
    def enabled(self) -> bool: ...

    @property
    def opacity(self) -> float: ...

    @property
    def invert(self) -> bool: ...

    @property
    def shape(self) -> LinearMask | RadialMask | BrushMask: ...

    @property
    def deltas(self) -> LocalDeltas: ...

    def __eq__(self, arg: LocalAdjustment, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> LocalAdjustment:
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

class NegativeSettings:
    """
    What shapes the decoded negative, as opposed to what is done with it: a RAW's demosaic and its lens corrections. Changing the demosaic costs a decode; the lens corrections are a stage of the render and cost none; no develop edit can reach either.
    """

    def __init__(self, *, demosaic: Demosaic = Demosaic.AHD, lens: LensSettings | None = None) -> None: ...

    @property
    def demosaic(self) -> Demosaic: ...

    @property
    def lens(self) -> LensSettings: ...

    def __eq__(self, arg: NegativeSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> NegativeSettings:
        """Return a copy with the given attributes replaced."""

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
    Everything that says how one photograph is developed: its global settings, and its local adjustments (masks), a tuple of LocalAdjustment with ids, in the order they sum in. `next_local_adjustment_id` is the id the next added mask takes; it never goes down.
    """

    def __init__(self, *, settings: DevelopSettings | None = None, local_adjustments: Sequence[LocalAdjustment] | None = None, next_local_adjustment_id: int | None = None) -> None: ...

    @property
    def settings(self) -> DevelopSettings: ...

    @property
    def local_adjustments(self) -> tuple[LocalAdjustment, ...]:
        """
        The masked adjustments, in the order they sum in: a tuple of LocalAdjustment.
        """

    @property
    def next_local_adjustment_id(self) -> int: ...

    def __eq__(self, arg: DevelopState, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> DevelopState:
        """Return a copy with the given attributes replaced."""

class SidecarContents:
    """
    What an XMP sidecar holds, and which other tools wrote in it. `state` is None when the sidecar records no develop settings (marks only, or another tool's), and `negative` when it records no negative settings.
    """

    def __init__(self, *, state: DevelopState | None = None, marks: PhotoMarks | None = None, negative: NegativeSettings | None = None, creator_tool: str | None = None, others: Sequence[ForeignNamespace] | None = None) -> None: ...

    @property
    def state(self) -> DevelopState | None: ...

    @property
    def marks(self) -> PhotoMarks: ...

    @property
    def negative(self) -> NegativeSettings | None: ...

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
    def negative(self) -> NegativeSettings:
        """
        How the negative is decoded (NegativeSettings); no develop edit changes it.
        """

    @property
    def marks(self) -> PhotoMarks: ...

    def edited(self, **kwargs) -> Photo:
        """
        Return a photograph with flat snake_case keywords set, e.g. exposure=0.7, by the rules the app and the command line apply: a temperature makes the white balance Custom, grain turned on gets a seed, a turn carries the crop, a straighten shrinks it, a crop rectangle frees the aspect. Keywords are applied in the order given, so crop_aspect then crop_rectangle differs from the reverse. Raises TypeError for an unknown keyword or a wrong type, and ValueError for a value the setting refuses or a geometry that does not fit the photograph's size.
        """

    @property
    def local_adjustments(self) -> tuple[LocalAdjustment, ...]:
        """
        The photograph's masked adjustments, ids included, in the order they sum in; the same as `state.local_adjustments`.
        """

    def add_linear_mask(self, from_: tuple[float, float], to: tuple[float, float], *, name: str = '', opacity: float = 1.0, invert: bool = False, enabled: bool = True, **deltas: float) -> Photo:
        """
        Return a photograph with a linear (graduated) mask added last, so that it is `local_adjustments[-1]`. `from_` and `to` are (u, v) points normalised to the corrected frame; the weight is 1 at `from_` and 0 at `to`. The remaining keywords are the mask's deltas by their snake_case names (relative_temperature, relative_tint, exposure, contrast, highlights, shadows, whites, blacks, texture, clarity, dehaze, saturation, vibrance), added to the global settings where the mask has weight. Numbers out of range are clamped; a non-finite or degenerate one, a full list (16) or an unknown keyword is refused (ValueError, TypeError).
        """

    def add_radial_mask(self, centre: tuple[float, float], radius_x: float, radius_y: float, angle: float = 0.0, feather: float = 0.5, *, name: str = '', opacity: float = 1.0, invert: bool = False, enabled: bool = True, **deltas: float) -> Photo:
        """
        Return a photograph with a radial (oval) mask added last. `centre` is a (u, v) point normalised to the corrected frame, the radii are in long-edge units, `angle` turns the x radius towards +y in degrees, and `feather` is the soft edge's width from 0 (hard) to 1. Keywords and errors are as for add_linear_mask.
        """

    def add_brush_mask(self, strokes: Sequence[Stroke] = (), *, name: str = '', opacity: float = 1.0, invert: bool = False, enabled: bool = True, **deltas: float) -> Photo:
        """
        Return a photograph with a brush mask of those strokes added last. Each stroke's numbers are clamped, then the list is built. A stroke without points, a number that is not finite, a cap or budget passed (see BrushMask) or a full list (16) is a ValueError, an unknown keyword a TypeError. Keywords are as for add_linear_mask.
        """

    def with_brush_stroke(self, id: int, stroke: Stroke) -> Photo:
        """
        Return a photograph with the stroke, clamped, appended to the brush mask of that id. ValueError for an unknown id, an adjustment that is not a brush, a stroke that cannot be normalised or a mask that would pass a cap or budget.
        """

    def with_local_adjustment(self, id: int, **changes: Any) -> Photo:
        """
        Return a photograph with the adjustment of that id changed. The keywords, applied in the order given, are `name`, `enabled`, `invert`, `opacity`, `shape` (a LinearMask, RadialMask or BrushMask of the kind the adjustment already has; a BrushMask replaces the strokes) and the deltas by their snake_case names; numbers out of range are clamped. ValueError for an unknown id or a shape of another kind, TypeError for an unknown keyword.
        """

    def without_local_adjustment(self, id: int) -> Photo:
        """
        Return a photograph without the adjustment of that id; its id is not reused. ValueError for an unknown id.
        """

    def load(self) -> ImageBuffer:
        """Decode the photograph's file into a buffer."""

    def __eq__(self, arg: Photo, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

    @property
    def lens(self) -> LensSettings:
        """The lens settings of the negative; the same as `negative.lens`."""

    def lens_correction(self, database: LensDatabase | None = None) -> LensCorrection:
        """
        Resolve the photograph's lens corrections as a render applies them: empty when none is on or the photograph is not a RAW, else the tables of the recorded lens (looked up by its exact name) or of the match of the file's EXIF. What cannot be resolved is logged on the 'arraw' logger and the correction skipped. `database` defaults to the process's, loaded only when a correction is on.
        """

    def lens_match(self, database: LensDatabase | None = None) -> LensMatch | None:
        """
        Find the lens the photograph's corrections are for, whether or not any is on: the recorded lens, else the match of the file's EXIF. None for a file that is not a RAW or when no lens is found (the reason is logged on the 'arraw' logger).
        """

    def with_lens_corrections(self, *, distortion: bool | None = None, vignetting_amount: float | None = None, lateral_ca: bool | None = None) -> Photo:
        """
        Return a photograph with the lens corrections given set, as `--lens-corrections` sets them; `vignetting_amount` is a number from 0 (off) to 150, 100 being the profile as it is (a bool is refused, ValueError out of range). None keeps one as it is, and the recorded lens is untouched (see record_lens_match). A photograph that is not a RAW is returned as it was.
        """

    def with_chosen_lens(self, maker: str, model: str, database: LensDatabase | None = None) -> Photo:
        """
        Return a photograph with the lens recorded as chosen by the photographer, as `--lens 'Maker|Model'` does: looked up by its exact lensfun name, with the camera's crop factor. ValueError when the database does not list it or the photograph is not a RAW.
        """

    def with_(
        self,
        state: DevelopState | None = None,
        *,
        marks: PhotoMarks | None = None,
        negative: NegativeSettings | None = None,
        rating: int = ...,
        label: ColorLabel | None = ...,
        **kwargs: Any,
    ) -> Photo:
        """
        Return a photograph with `state` (and `marks`, and `negative`) replacing the current ones wholesale, then flat snake_case keywords applied, e.g. exposure=0.7, which edit the settings of the state. `rating` and `label` change the marks instead (label=None clears it). Applies no rules: each keyword is assigned as it is, so a turn leaves the crop where it was. `edited` applies the rules of the editing frontends.
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
def develop(source: ImageBuffer, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False, lens: LensCorrection | None = None) -> ImageBuffer:
    """
    Develop a decoded buffer on the CPU; with no state, the defaults of its kind (default_state: colour noise reduction for a RAW, nothing for anything else). `size` renders the cropped result smaller: an int is the long edge, a (width, height) tuple a box to fit inside, a float a scale factor. Sizes only shrink unless `allow_upscale`. `lens` is the correction to apply to the sensor frame, as Photo.lens_correction resolves it; None applies none, since a buffer knows nothing of its photograph.
    """

@overload
def develop(source: Photo, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False, lens_database: LensDatabase | None = None) -> ImageBuffer:
    """
    Decode a photograph and develop it with its own state unless `state` is given; `size`, `filter` and `allow_upscale` are as for a decoded buffer. The photograph's own lens corrections are applied when it asks for any (Photo.lens_correction), looked up in `lens_database`, by default the process's; what cannot be resolved is logged on the 'arraw' logger and the render goes ahead without it.
    """

class Tap(enum.Enum):
    """Named position inside the pointwise chain that sample() stops at."""

    CURVE_INPUT = 0
    """
    What the tone curves take in: after white balance, exposure and Basic Tone; handed back in NamedEncoding.REC2020_GAMMA22.
    """

@overload
def sample(source: ImageBuffer, tap: Tap, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False, lens: LensCorrection | None = None) -> ImageBuffer:
    """
    Develop a decoded buffer on the CPU with the chain stopped at `tap`, to measure it: the same frame and size as develop() with the same arguments, in the tap's encoding.
    """

@overload
def sample(source: Photo, tap: Tap, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = None, filter: ResizeFilter = ResizeFilter.LANCZOS3, allow_upscale: bool = False, lens_database: LensDatabase | None = None) -> ImageBuffer:
    """
    Decode a photograph and sample it at `tap` with its own state unless `state` is given, through its own lens corrections as develop() applies them.
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
def curve_histogram(source: ImageBuffer, state: DevelopState, *, size: int | tuple[int, int] | float | None = 1024, allow_upscale: bool = False, lens: LensCorrection | None = None) -> CurveHistogram:
    """
    Sample a decoded buffer at Tap.CURVE_INPUT and count it. `size` is as for develop() and defaults to a 1024-pixel long edge; None counts the full cropped resolution. The resize is always bilinear, so no ringing reaches the end bins.
    """

@overload
def curve_histogram(source: Photo, state: DevelopState | None = None, *, size: int | tuple[int, int] | float | None = 1024, allow_upscale: bool = False, lens_database: LensDatabase | None = None) -> CurveHistogram:
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

def save(image: ImageBuffer, path: str | os.PathLike, *, format: ImageFileFormat | None = None, encoding: NamedEncoding = ..., bit_depth: int = 8, quality: int = 90, embed_profile: bool = True, sharpening: int = 0, metadata_from: Photo | None = None, metadata: MetadataSelection = ...) -> None:
    """
    Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given. `sharpening` (0-100, default 0 = off) applies an unsharp mask to the final pixels. With `metadata_from` (the photograph the pixels came from) the groups of `metadata` are copied from its file and sidecar, and its marks written as rating and label; without it nothing is written. A source or sidecar that cannot be read is logged as a warning on the 'arraw' logger and its metadata left out; the file is still written.
    """

class ExportSettings:
    """
    What an export is asked for: file format, output colour, 16 bits or 8, the box to fit (`resize` with `width` and `height`, which `allow_enlarging` lets exceed the photograph's own size), JPEG `quality`, output `sharpening`, and the metadata groups (`capture_info`, `location`, `descriptive`) the file carries. Presets are named ExportSettings; `develop_size` gives the `size` for `develop`.
    """

    def __init__(self, *, format: ImageFileFormat = ImageFileFormat.JPEG, encoding: NamedEncoding = NamedEncoding.SRGB, sixteen_bit: bool = False, resize: bool = False, width: int | None = 0, height: int | None = 0, allow_enlarging: bool = False, quality: int | None = 90, sharpening: int | None = 0, capture_info: bool = True, location: bool = False, descriptive: bool = True) -> None: ...

    @property
    def format(self) -> ImageFileFormat: ...

    @property
    def encoding(self) -> NamedEncoding: ...

    @property
    def sixteen_bit(self) -> bool: ...

    @property
    def resize(self) -> bool: ...

    @property
    def width(self) -> int: ...

    @property
    def height(self) -> int: ...

    @property
    def allow_enlarging(self) -> bool: ...

    @property
    def quality(self) -> int: ...

    @property
    def sharpening(self) -> int: ...

    @property
    def capture_info(self) -> bool: ...

    @property
    def location(self) -> bool: ...

    @property
    def descriptive(self) -> bool: ...

    def __eq__(self, arg: ExportSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ExportSettings:
        """Return a copy with the given attributes replaced."""

    @property
    def develop_size(self) -> tuple[int, int] | None:
        """
        The `size` to give `develop`: the box to fit as (width, height), or None for the photograph's own size.
        """

    def canonical(self) -> ExportSettings:
        """
        The settings without what the format and size do not use: width, height and enlarging are 0 and False when not resizing, and 16 bits is off for JPEG. Two settings that export alike have one canonical form.
        """

    def validate(self) -> None:
        """
        Raise ValueError unless the settings can be exported: quality and sharpening 0 to 100, a box of 1 to 99999 on each side when resizing.
        """

class ExportPresetProblem(enum.Enum):
    """Why a change to the export presets did not happen."""

    INVALID_NAME = 0
    """The name is empty, too long or holds control characters."""

    NAME_TAKEN = 1
    """Another preset already has the new name."""

    NOT_FOUND = 2
    """No preset has the name."""

    UNUSABLE = 3
    """The preset is one this arraw cannot use."""

    NEWER_FILE = 4
    """A newer arraw wrote the file, so it is never written."""

    UNREADABLE_FILE = 5
    """The file cannot be read, so it is never written over."""

    BUSY = 6
    """Another process is changing the file and did not finish in time."""

    WRITE_FAILED = 7
    """The file or its directory could not be written; the old file stays."""

    BUILT_IN = 8
    """The name is a built-in preset's, which cannot be changed."""

    NOT_UNREADABLE = 9
    """
    The file can be read, so it is not set aside: only an unreadable one is.
    """

class ExportPresetFileState(enum.Enum):
    """What reading the export presets file found."""

    MISSING = 0
    """There is no file."""

    READ = 1
    """The file was read, empty or not."""

    NEWER = 2
    """A newer arraw wrote it: read as far as understood, never written."""

    UNREADABLE = 3
    """It cannot be read: no presets from it, and it is never written over."""

class ExportPresetError(RuntimeError):
    """
    A change to the export presets that could not be made. `problem` says why (ExportPresetProblem); the message names the file.
    """

class ExportPreset:
    """
    A named, whole set of export settings. `builtin` presets are defined by arraw and read-only; `shadowed` is a preset of the file whose name a built-in also has, which the built-in hides.
    """

    def __init__(self, name: str, settings: ExportSettings | None = None) -> None:
        """
        A preset for saving; `settings` default to those of a new ExportSettings.
        """

    @property
    def name(self) -> str: ...

    @property
    def settings(self) -> ExportSettings: ...

    @property
    def builtin(self) -> bool: ...

    @property
    def shadowed(self) -> bool: ...

    def __eq__(self, arg: ExportPreset, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def to_json(self) -> str:
        """
        The preset as the presets file holds it and `arraw-cli export-preset show` prints it: a JSON object, every key written. Raises ValueError for a name or settings that are not valid.
        """

class UnusableExportPreset:
    """
    A preset in the file that this arraw cannot use, kept in the file as it is.
    """

    @property
    def name(self) -> str:
        """Its name as written, empty when it has none."""

    @property
    def reason(self) -> str:
        """Why it cannot be used, as a phrase."""

    def __repr__(self) -> str: ...

class ExportPresetStore:
    """
    The export presets file of one directory, read and changed safely. Reading takes no lock and creates nothing. Each change takes a lock file, re-reads the file and replaces it atomically, so that the app, the command line and scripts lose none of each other's changes. A file written by a newer arraw, or one that cannot be read, is never written. The built-in presets come first in `presets()` and cannot be changed. Changes raise ExportPresetError.
    """

    def __init__(self, directory: str | os.PathLike | None = None) -> None:
        """
        A store for the presets file of `directory`, which need not exist; None is the configuration directory (config_directory(), which ARRAW_CONFIG_DIR overrides).
        """

    @property
    def file(self) -> pathlib.Path:
        """Path of the presets file, which may not exist."""

    def presets(self) -> list[ExportPreset]:
        """
        The usable presets: the built-in ones first, in their order, then the file's, sorted by name. Problems of the file are logged on the 'arraw' logger.
        """

    def unusable(self) -> list[UnusableExportPreset]:
        """The entries of the file that this arraw cannot use."""

    def state(self) -> ExportPresetFileState:
        """
        What the file is: ExportPresetFileState.MISSING, READ, NEWER or UNREADABLE.
        """

    def load(self, name: str) -> ExportPreset:
        """
        Find a preset by name, case-insensitively; a built-in wins over a preset of the file that has its name. Raises ExportPresetError with NOT_FOUND when there is none, or UNUSABLE when the file's preset of that name cannot be used by this arraw.
        """

    def save(self, preset: ExportPreset) -> None:
        """
        Add a preset, or replace the one whose name folds alike (the new spelling is kept). Raises ExportPresetError (INVALID_NAME, BUILT_IN, UNUSABLE, NEWER_FILE, UNREADABLE_FILE, BUSY or WRITE_FAILED), or ValueError for invalid settings.
        """

    def rename(self, old: str, new: str) -> None:
        """
        Give a preset another name; a change of letter case only is allowed. Raises ExportPresetError (NOT_FOUND, NAME_TAKEN and the problems of save).
        """

    def delete(self, name: str) -> None:
        """
        Remove a preset, usable or not. Raises ExportPresetError with NOT_FOUND when there is none, or BUILT_IN for a built-in one.
        """

    def set_aside(self) -> pathlib.Path:
        """
        Move the presets file to `export-presets.unreadable-<date>.json` beside it, so that the next change starts a new file, and return the new path. Never replaces a file. Only an unreadable file is moved: raises ExportPresetError with NOT_FOUND when there is none, NEWER_FILE for a newer arraw's, NOT_UNREADABLE for one that reads fine.
        """

def config_directory() -> pathlib.Path:
    """
    The directory where arraw keeps its shared configuration: ARRAW_CONFIG_DIR when set, else `arraw` under the user's generic configuration location. Nothing is created. Raises RuntimeError when there is none.
    """

def record_lens_match(photo: Photo, database: LensDatabase | None = None) -> Photo:
    """
    Record the lens matched from the file's EXIF in a photograph that has a correction on and no lens recorded, with its crop factor and the database's stamp, so that a database update cannot move the edit to another lens. The photograph is returned as it was when nothing is on, a lens is recorded already, it is not a RAW, or no lens matches.
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

    @property
    def negative(self) -> NegativeSettings:
        """Negative settings after the step."""

    @property
    def lens(self) -> LensSettings:
        """Lens settings after the step; the same as `negative.lens`."""

    def __eq__(self, arg: HistoryStep, /) -> bool: ...

    def __repr__(self) -> str: ...

class ChangeDescription:
    """Settings two develop states differ in, for naming a step."""

    @property
    def keys(self) -> list[str]:
        """
        Descriptor keys (camelCase, as JSON and the sidecar spell them) whose values differ, in table order; setting_descriptors() maps them to the Python names.
        """

    @property
    def group(self) -> SettingGroup | None:
        """
        Group every key belongs to; None when there are no keys or they span groups.
        """

    @property
    def negative(self) -> list[str]:
        """
        Keys of the negative settings that differ, in the order 'demosaic', 'lensDistortion', 'lensVignetting', 'lensLateralCa' and 'lens' (the recorded lens); empty unless made by describe_change over two history steps.
        """

    @property
    def lens(self) -> list[str]:
        """
        The lens keys of `negative`: which corrections were switched and whether the recorded lens changed.
        """

    def __eq__(self, arg: ChangeDescription, /) -> bool: ...

    def __repr__(self) -> str: ...

@overload
def describe_change(before: DevelopState, after: DevelopState) -> ChangeDescription:
    """List the settings two develop states differ in."""

@overload
def describe_change(before: HistoryStep, after: HistoryStep) -> ChangeDescription:
    """
    List what two history steps differ in: the develop state, and the negative settings in `negative`.
    """

class EditScope:
    """Context manager of one edit; made by EditSession.edit()."""

    def __enter__(self) -> object: ...

    def __exit__(self, type: object | None, value: object | None, traceback: object | None) -> bool: ...

class EditSession:
    """
    One photograph being edited: the document as it stands, and how it got there.
    """

    def __init__(self, photo: Photo, lens_database: LensDatabase | None = None) -> None:
        """
        Start editing `photo`. `lens_database` is the database the session records lens matches in (None: the process's, loaded on first use, for a RAW only; other files have none). It is kept alive as long as the session.
        """

    @property
    def lens_database(self) -> LensDatabase | None:
        """The lens database the session records matches in, or None (not a RAW)."""

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

    def set_state(self, state: DevelopState, origin: EditOrigin = EditOrigin.EDIT, detail: str = '', *, negative: NegativeSettings | None = None) -> None:
        """
        Replace the develop state as one history step. With `negative`, the negative settings change in the same step (a paste or a preset); None leaves them.
        """

    def set_negative(self, negative: NegativeSettings, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """
        Replace the negative settings as one history step; equal ones leave none. An open edit is committed first.
        """

    def update_negative(self, negative: NegativeSettings) -> None:
        """
        Change the negative settings provisionally, inside the open edit, so that a drag of a lens amount is one history step on commit. RuntimeError when no edit is open and ValueError when the settings are invalid, with nothing changed.
        """

    def set_lens_corrections(self, *, distortion: bool | None = None, vignetting_amount: float | None = None, lateral_ca: bool | None = None, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """
        Set the lens corrections given (None keeps one) as one history step; `vignetting_amount` is a number from 0 to 150 (100 is the profile; a bool is refused). A correction turned on for a photograph with no recorded lens records the lens matched from its EXIF in the same step, in the session's `lens_database`, so undo takes both away; switching off keeps the record. Equal settings leave no step.
        """

    def choose_lens(self, maker: str, model: str, database: LensDatabase | None = None, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """
        Record the lens the photographer chose, as one history step; ValueError when the database (default: the session's, else the process's) does not list it. The switches stay as they are.
        """

    def reset_lens(self, origin: EditOrigin = EditOrigin.EDIT, detail: str = '') -> None:
        """
        Forget the recorded lens, as one history step; the session matches the file's EXIF again when a correction is on ("reset to automatic").
        """

    @property
    def history(self) -> list[HistoryStep]:
        """
        Steps taken so far, the opening state first; a new list on every access.
        """

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
