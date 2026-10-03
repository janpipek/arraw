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


def test_stub_imports_only_real_modules():
    # Stubgen reads a dotted default in a hand-written signature as a module
    # path and emits a bogus `import ResizeFilter`.
    tree = ast.parse(STUB.read_text())
    imported = set()
    for node in tree.body:
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            imported.add(node.module.split(".")[0])
    classes = {n.name for n in tree.body if isinstance(n, ast.ClassDef)}
    assert not imported & classes
    allowed = {"collections", "enum", "os", "pathlib", "typing", "numpy", "types", "__future__"}
    assert imported <= allowed, imported - allowed
