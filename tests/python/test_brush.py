"""The brush's value types in Python and a state that holds one (ADR 044, section 10).

Step 5.1: the types, so that a state holding a brush converts and renders on the CPU. Step 5.4: it
persists. Step 5.5: the Photo methods (add_brush_mask, with_brush_stroke)."""

import math
import shutil

import numpy as np
import pytest

import arraw


def some_stroke(v=0.5, **style):
    return arraw.Stroke(points=[(0.2, v), (0.5, v), (0.8, v)], **style)


def state_with(photo, mask, invert=False, **deltas):
    """The photo's own state with one masked adjustment added."""
    adjustment = arraw.LocalAdjustment(
        id=1, shape=mask, invert=invert, deltas=arraw.LocalDeltas(**deltas)
    )
    return arraw.DevelopState(settings=photo.state.settings, local_adjustments=[adjustment])


@pytest.fixture
def photo(dng):
    return arraw.open(dng, sidecar=False)


# ---- Stroke ----------------------------------------------------------------------------------


def test_stroke_defaults_and_attributes():
    stroke = arraw.Stroke()
    assert (stroke.radius, stroke.hardness, stroke.flow, stroke.erase) == (0.02, 0.5, 1.0, False)
    assert stroke.points == ()
    stroke = arraw.Stroke(0.1, 0.25, 0.5, True, [(0.1, 0.2), [0.3, 0.4]])
    assert (stroke.radius, stroke.hardness, stroke.flow, stroke.erase) == (0.1, 0.25, 0.5, True)
    assert stroke.points == ((0.1, 0.2), (0.3, 0.4))
    assert isinstance(stroke.points, tuple)
    assert all(isinstance(point, tuple) for point in stroke.points)


def test_stroke_keeps_the_numbers_as_given():
    stroke = arraw.Stroke(radius=7.0, hardness=-2.0, points=[(10.0, -10.0)])
    assert (stroke.radius, stroke.hardness, stroke.points) == (7.0, -2.0, ((10.0, -10.0),))


def test_stroke_refuses_what_is_no_number_or_pair():
    for bad in [True, "0.1", None]:
        with pytest.raises(TypeError):
            arraw.Stroke(radius=bad)
    for bad in [[(0.5,)], [(0.1, 0.2, 0.3)], ["ab"], [(True, 0.5)], 3]:
        with pytest.raises(TypeError):
            arraw.Stroke(points=bad)
    with pytest.raises(TypeError):
        arraw.Stroke(erase=1)


def test_stroke_is_frozen_hashable_and_compares_by_value():
    stroke = some_stroke(radius=0.1)
    with pytest.raises(AttributeError):
        stroke.radius = 0.2
    assert stroke == some_stroke(radius=0.1)
    assert stroke != some_stroke(radius=0.2)
    assert stroke != some_stroke(v=0.6, radius=0.1)
    assert hash(stroke) == hash(some_stroke(radius=0.1))
    assert stroke.replace(erase=True).erase is True
    assert stroke.replace(points=[(0.5, 0.5)]).points == ((0.5, 0.5),)
    with pytest.raises(TypeError):
        stroke.replace(colour=1)
    assert "3 points" in repr(stroke)


# ---- BrushMask -------------------------------------------------------------------------------


def test_brush_mask_defaults_and_attributes():
    empty = arraw.BrushMask()
    assert empty.strokes == ()
    assert empty.rasteriser == 1
    mask = arraw.BrushMask([some_stroke(), some_stroke(0.7)])
    assert isinstance(mask.strokes, tuple)
    assert mask.strokes == (some_stroke(), some_stroke(0.7))
    assert mask.rasteriser == 1


def test_brush_mask_clamps_each_stroke_as_adding_a_mask_does():
    wild = arraw.Stroke(radius=9.0, hardness=3.0, flow=-1.0, points=[(100.0, -100.0)])
    (stroke,) = arraw.BrushMask([wild]).strokes
    assert stroke.radius == 1.0
    assert stroke.hardness == 1.0
    assert stroke.flow == 0.0
    assert stroke.points == ((3.0, -2.0),)


def test_brush_mask_refuses_non_finite_numbers_and_budgets():
    for bad in [math.nan, math.inf]:
        with pytest.raises(ValueError):
            arraw.BrushMask([arraw.Stroke(radius=bad, points=[(0.5, 0.5)])])
        with pytest.raises(ValueError):
            arraw.BrushMask([arraw.Stroke(points=[(bad, 0.5)])])
    with pytest.raises(ValueError):
        arraw.BrushMask([arraw.Stroke()])  # no points
    with pytest.raises(ValueError):
        arraw.BrushMask([arraw.Stroke(points=[(0.5, 0.5)] * 10_001)])
    with pytest.raises(ValueError):
        arraw.BrushMask([arraw.Stroke(points=[(0.5, 0.5)])] * 2_001)
    sweep = arraw.Stroke(radius=1.0, points=[(0.0, 0.0), (3.0, 0.0)])
    arraw.BrushMask([sweep])
    with pytest.raises(ValueError):
        arraw.BrushMask([sweep, sweep])
    with pytest.raises(TypeError):
        arraw.BrushMask([1])


def test_brush_mask_is_frozen_hashable_equal_by_contents_with_a_short_repr():
    mask = arraw.BrushMask([some_stroke(), some_stroke(0.7)])
    with pytest.raises(AttributeError):
        mask.strokes = ()
    again = arraw.BrushMask((some_stroke(), some_stroke(0.7)))
    assert mask == again
    assert hash(mask) == hash(again)
    assert mask != arraw.BrushMask([some_stroke()])
    assert mask != arraw.LinearMask()
    assert repr(mask) == "BrushMask(2 strokes, 6 points)"
    assert repr(arraw.BrushMask()) == "BrushMask(0 strokes, 0 points)"


def test_a_local_adjustment_holds_a_brush():
    mask = arraw.BrushMask([some_stroke()])
    adjustment = arraw.LocalAdjustment(shape=mask)
    assert adjustment.shape == mask
    assert isinstance(adjustment.shape, arraw.BrushMask)
    assert adjustment == arraw.LocalAdjustment(shape=arraw.BrushMask([some_stroke()]))
    assert adjustment != arraw.LocalAdjustment(shape=arraw.RadialMask())
    assert hash(adjustment) == hash(arraw.LocalAdjustment(shape=arraw.BrushMask([some_stroke()])))


# ---- a state holding a brush -----------------------------------------------------------------


def test_a_state_with_a_brush_renders_on_the_cpu(photo):
    plain = arraw.develop(photo).pixels
    mask = arraw.BrushMask(
        [arraw.Stroke(radius=0.3, hardness=1.0, points=[(0.2, 0.5), (0.8, 0.5)])]
    )
    brushed = photo.with_(state_with(photo, mask, exposure=1.0))
    pixels = arraw.develop(brushed).pixels
    assert pixels.shape == plain.shape
    assert not np.array_equal(pixels, plain)
    # Under the stroke it is brighter, far from it the render is the plain one.
    height, width = plain.shape[:2]
    assert pixels[height // 2, width // 2].sum() > plain[height // 2, width // 2].sum()
    assert np.array_equal(pixels[0, 0], plain[0, 0])


def test_an_empty_brush_does_nothing_and_inverted_is_global(photo):
    plain = arraw.develop(photo).pixels
    empty = photo.with_(state_with(photo, arraw.BrushMask(), exposure=1.0))
    # A mask that reaches nothing leaves the render bit for bit (ADR 044).
    assert np.array_equal(arraw.develop(empty).pixels, plain)
    inverted = photo.with_(state_with(photo, arraw.BrushMask(), invert=True, exposure=1.0))
    base = photo.state.settings
    brighter = base.with_(exposure=base.tone.exposure + 1.0)
    expected = photo.with_(arraw.DevelopState(settings=brighter))
    assert np.array_equal(arraw.develop(inverted).pixels, arraw.develop(expected).pixels)
    assert not np.array_equal(arraw.develop(inverted).pixels, plain)


def test_a_photo_edit_by_shape_keeps_the_kind(photo):
    masked = photo.with_(state_with(photo, arraw.BrushMask([some_stroke()]), exposure=1.0))
    (adjustment,) = masked.local_adjustments
    replaced = masked.with_local_adjustment(adjustment.id, shape=arraw.BrushMask([some_stroke(0.8)]))
    assert replaced.local_adjustments[0].shape == arraw.BrushMask([some_stroke(0.8)])
    with pytest.raises(ValueError):
        masked.with_local_adjustment(adjustment.id, shape=arraw.LinearMask())


def test_a_brush_survives_a_sidecar_round_trip(dng, tmp_path):
    copy = tmp_path / "copy.dng"
    shutil.copy(dng, copy)
    photo = arraw.open(copy, sidecar=False)
    masked = photo.with_(state_with(photo, arraw.BrushMask([some_stroke()]), exposure=1.0))
    arraw.write_sidecar(masked)
    reloaded = arraw.open(copy)
    assert reloaded.local_adjustments == masked.local_adjustments
    assert np.array_equal(arraw.develop(reloaded).pixels, arraw.develop(masked).pixels)


# ---- the Photo methods -----------------------------------------------------------------------


def test_add_brush_mask_adds_last_with_name_opacity_and_deltas(photo):
    added = photo.add_brush_mask(
        [some_stroke()], name="Sky", opacity=0.5, invert=True, enabled=False, exposure=0.5
    )
    (adjustment,) = added.local_adjustments
    assert adjustment.shape == arraw.BrushMask([some_stroke()])
    assert (adjustment.name, adjustment.opacity) == ("Sky", 0.5)
    assert adjustment.invert and not adjustment.enabled
    assert adjustment.deltas.exposure == 0.5
    assert adjustment.id == 1
    again = added.add_brush_mask()
    assert again.local_adjustments[-1].shape == arraw.BrushMask()
    assert again.local_adjustments[-1].id == 2
    assert photo.local_adjustments == ()


def test_add_brush_mask_clamps_each_stroke(photo):
    wild = arraw.Stroke(radius=9.0, hardness=3.0, flow=-1.0, points=[(100.0, -100.0)])
    added = photo.add_brush_mask([wild], exposure=99.0)
    (stroke,) = added.local_adjustments[0].shape.strokes
    assert (stroke.radius, stroke.hardness, stroke.flow) == (1.0, 1.0, 0.0)
    assert stroke.points == ((3.0, -2.0),)
    assert added.local_adjustments[0].deltas.exposure == photo.add_linear_mask(
        (0.5, 0.0), (0.5, 1.0), exposure=99.0
    ).local_adjustments[0].deltas.exposure
    assert added.local_adjustments[0].shape == arraw.BrushMask([wild])


def test_add_brush_mask_refuses_bad_input(photo):
    with pytest.raises(ValueError):
        photo.add_brush_mask([arraw.Stroke()])  # no points
    with pytest.raises(ValueError):
        photo.add_brush_mask([arraw.Stroke(radius=math.nan, points=[(0.5, 0.5)])])
    with pytest.raises(ValueError):
        photo.add_brush_mask([arraw.Stroke(points=[(0.5, 0.5)])] * 2_001)
    sweep = arraw.Stroke(radius=1.0, points=[(0.0, 0.0), (3.0, 0.0)])
    with pytest.raises(ValueError):
        photo.add_brush_mask([sweep, sweep])
    with pytest.raises(ValueError):
        photo.add_brush_mask([some_stroke()], exposure=math.nan)
    with pytest.raises(TypeError):
        photo.add_brush_mask([some_stroke()], bogus=1.0)
    with pytest.raises(TypeError):
        photo.add_brush_mask([1])


def test_add_brush_mask_refuses_the_seventeenth(photo):
    full = photo
    for _ in range(16):
        full = full.add_brush_mask([some_stroke()])
    assert len(full.local_adjustments) == 16
    with pytest.raises(ValueError):
        full.add_brush_mask([some_stroke()])


def test_with_brush_stroke_appends_a_clamped_stroke(photo):
    base = photo.add_brush_mask([some_stroke()], exposure=1.0)
    (adjustment,) = base.local_adjustments
    extended = base.with_brush_stroke(adjustment.id, some_stroke(0.8, radius=5.0))
    strokes = extended.local_adjustments[0].shape.strokes
    assert len(strokes) == 2
    assert strokes[0] == some_stroke()
    assert strokes[1] == some_stroke(0.8, radius=1.0)
    assert len(base.local_adjustments[0].shape.strokes) == 1
    assert extended.local_adjustments[0].deltas == adjustment.deltas


def test_with_brush_stroke_refuses_what_cannot_be_appended(photo):
    base = photo.add_brush_mask([some_stroke()]).add_linear_mask((0.5, 0.0), (0.5, 1.0))
    brush, linear = base.local_adjustments
    with pytest.raises(ValueError):
        base.with_brush_stroke(linear.id, some_stroke())  # not a brush
    with pytest.raises(ValueError):
        base.with_brush_stroke(99, some_stroke())  # unknown id
    with pytest.raises(ValueError):
        base.with_brush_stroke(brush.id, arraw.Stroke())  # no points
    with pytest.raises(ValueError):
        base.with_brush_stroke(brush.id, arraw.Stroke(radius=math.inf, points=[(0.5, 0.5)]))
    full = photo.add_brush_mask([arraw.Stroke(points=[(0.5, 0.5)])] * 2_000)
    with pytest.raises(ValueError):
        full.with_brush_stroke(full.local_adjustments[0].id, arraw.Stroke(points=[(0.5, 0.5)]))
    with pytest.raises(TypeError):
        base.with_brush_stroke(brush.id, (0.5, 0.5))


def test_painting_by_strokes_matches_a_brush_made_at_once(photo):
    first, second = some_stroke(0.3, radius=0.2), some_stroke(0.7, radius=0.2, erase=True)
    one = photo.add_brush_mask([first, second], exposure=1.0)
    two = photo.add_brush_mask([first], exposure=1.0)
    two = two.with_brush_stroke(two.local_adjustments[0].id, second)
    assert one.local_adjustments == two.local_adjustments
    assert np.array_equal(arraw.develop(one).pixels, arraw.develop(two).pixels)


def test_a_python_made_brush_renders_and_repeated_renders_agree(photo):
    plain = arraw.develop(photo).pixels
    brushed = photo.add_brush_mask(
        [arraw.Stroke(radius=0.3, hardness=1.0, points=[(0.2, 0.5), (0.8, 0.5)])], exposure=1.0
    )
    first = arraw.develop(brushed).pixels
    assert not np.array_equal(first, plain)
    # Repeated renders, cached or not, must not differ.
    assert np.array_equal(arraw.develop(brushed).pixels, first)
    small = arraw.develop(brushed, size=16).pixels
    assert np.array_equal(arraw.develop(brushed, size=16).pixels, small)


def test_a_brush_added_by_the_method_survives_a_sidecar(dng, tmp_path):
    copy = tmp_path / "copy.dng"
    shutil.copy(dng, copy)
    photo = arraw.open(copy, sidecar=False)
    masked = photo.add_brush_mask([some_stroke(), some_stroke(0.7, erase=True)], exposure=1.0)
    masked = masked.add_radial_mask((0.5, 0.5), 0.2, 0.1, exposure=-0.5)
    arraw.write_sidecar(masked)
    reloaded = arraw.open(copy)
    assert reloaded.local_adjustments == masked.local_adjustments
    assert np.array_equal(arraw.develop(reloaded).pixels, arraw.develop(masked).pixels)
