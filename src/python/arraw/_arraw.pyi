"""Python bindings for the arraw RAW processing engine."""

import enum
import os
import pathlib
from typing import overload

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
    def pixels(self) -> NDArray:
        """
        Writable NumPy view of shape (height, width, channels), without a copy; it keeps the buffer alive.
        """

    def __repr__(self) -> str: ...

def read_metadata(path: str | os.PathLike) -> ImageMetadata:
    """Read what a file declares about itself, without decoding its pixels."""

def load(path: str | os.PathLike) -> ImageBuffer:
    """Decode an image file into a buffer."""

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

class Applicability(enum.Enum):
    """Whether a setting means anything for every photograph."""

    ALWAYS = 0

    RAW_ONLY = 1

class Stage(enum.Enum):
    """Pass boundary of the render pipeline."""

    POINTWISE = 0

    GEOMETRY = 1

class ToneSettings:
    """Photographic tone adjustments."""

    def __init__(self, *, exposure: float = 0.0, contrast: float = 0.0, shadows: float = 0.0, highlights: float = 0.0, blacks: float = 0.0, whites: float = 0.0, filmic_highlights: float = 25.0) -> None: ...

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

    def __init__(self, *, white_balance: WhiteBalanceMode = WhiteBalanceMode.AS_SHOT, temperature: float | None = None, tint: float | None = None) -> None: ...

    @property
    def white_balance(self) -> WhiteBalanceMode: ...

    @property
    def temperature(self) -> float | None: ...

    @property
    def tint(self) -> float | None: ...

    def __eq__(self, arg: ColorSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> ColorSettings:
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

    def __init__(self, width_over_height: float = 1.0) -> None: ...

    @property
    def width_over_height(self) -> float: ...

    def __eq__(self, arg: CropRatio, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> CropRatio:
        """Return a copy with the given attributes replaced."""

class UprightCropRect:
    """Crop edges normalised to the uncropped upright rectangle."""

    def __init__(self, left: float = 0.0, top: float = 0.0, right: float = 1.0, bottom: float = 1.0) -> None: ...

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

    def __init__(self, *, rotation: QuarterTurn = QuarterTurn.NONE, flip_horizontal: bool = False, flip_vertical: bool = False, straighten: float = 0.0, crop: CropSettings | None = None) -> None: ...

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

class DevelopSettings:
    """Photographic settings of one photograph."""

    def __init__(self, *, color: ColorSettings | None = None, geometry: GeometrySettings | None = None, tone: ToneSettings | None = None) -> None: ...

    @property
    def color(self) -> ColorSettings: ...

    @property
    def geometry(self) -> GeometrySettings: ...

    @property
    def tone(self) -> ToneSettings: ...

    def __eq__(self, arg: DevelopSettings, /) -> bool: ...

    def __hash__(self) -> int: ...

    def __repr__(self) -> str: ...

    def replace(self, **kwargs) -> DevelopSettings:
        """Return a copy with the given attributes replaced."""

    def with_(self, **kwargs: Any) -> DevelopSettings:
        """Return a copy with flat snake_case keywords applied, e.g. exposure=0.7."""

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

    def __eq__(self, arg: SettingDescriptor, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

def setting_descriptors() -> list[SettingDescriptor]:
    """
    List the develop settings: key, snake_case name, range, group, applicability, stage.
    """

class Severity(enum.Enum):
    """How serious a diagnostic is."""

    INFO = 0

    WARNING = 1

    ERROR = 2

class ImageFileFormat(enum.Enum):
    """File format an image can be saved as."""

    JPEG = 0

    PNG = 1

    TIFF = 2

class Photo:
    """One photograph as a document: a file and how it is developed."""

    @property
    def path(self) -> pathlib.Path: ...

    @property
    def metadata(self) -> ImageMetadata: ...

    @property
    def settings(self) -> DevelopSettings: ...

    def load(self) -> ImageBuffer:
        """Decode the photograph's file into a buffer."""

    def __eq__(self, arg: Photo, /) -> bool: ...

    def __repr__(self) -> str: ...

    __hash__: None = None

    def with_(self, settings: DevelopSettings | None = None, **kwargs: Any) -> Photo:
        """
        Return a photograph with `settings` replacing the current ones wholesale, then flat snake_case keywords applied, e.g. exposure=0.7.
        """

def open(path: str | os.PathLike) -> Photo:
    """Open a photograph; reads its metadata, not its pixels."""

@overload
def develop(source: ImageBuffer, settings: DevelopSettings | None = None) -> ImageBuffer:
    """
    Develop a decoded buffer on the CPU; default settings leave the colour unchanged.
    """

@overload
def develop(source: Photo, settings: DevelopSettings | None = None) -> ImageBuffer:
    """
    Decode a photograph and develop it with its own settings unless `settings` is given.
    """

def save(image: ImageBuffer, path: str | os.PathLike, *, format: ImageFileFormat | None = None, encoding: NamedEncoding = NamedEncoding.SRGB, bit_depth: int = 8, quality: int = 90, embed_profile: bool = True) -> None:
    """
    Write an image as JPEG, PNG or TIFF; the format comes from the extension unless given.
    """

__version__: str
