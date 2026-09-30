import ast

from conftest import ROOT

STUB = ROOT / "src" / "python" / "arraw" / "_arraw.pyi"


def members(tree, class_name):
    (cls,) = [n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == class_name]
    return {n.name for n in cls.body if isinstance(n, ast.FunctionDef)}


def test_stub_declares_what_the_generator_skips():
    tree = ast.parse(STUB.read_text())
    assert "with_" in members(tree, "Photo")
    assert "with_" in members(tree, "DevelopSettings")
    names = {n.target.id for n in tree.body if isinstance(n, ast.AnnAssign)}
    assert "__version__" in names
