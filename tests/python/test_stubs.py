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
    allowed = {"collections", "datetime", "enum", "os", "pathlib", "typing", "numpy", "types", "__future__"}
    assert imported <= allowed, imported - allowed


def test_stub_declares_every_exported_name_of_the_extension():
    import arraw

    tree = ast.parse(STUB.read_text())
    declared = {n.name for n in tree.body if isinstance(n, (ast.ClassDef, ast.FunctionDef))}
    declared |= {n.target.id for n in tree.body if isinstance(n, ast.AnnAssign)}
    native = {
        name
        for name in arraw.__all__
        if getattr(getattr(arraw, name), "__module__", None) == "arraw._arraw"
    }
    assert {"ToneCurve", "ToneCurveSettings", "GradeZone", "ColorGradingSettings",
            "VignetteSettings", "EffectsSettings"} <= native
    assert native <= declared, native - declared


def test_stub_annotations_only_use_defined_names():
    import builtins

    tree = ast.parse(STUB.read_text())
    defined = set(dir(builtins))
    for node in ast.walk(tree):
        if isinstance(node, (ast.ClassDef, ast.FunctionDef)):
            defined.add(node.name)
        elif isinstance(node, ast.Import):
            defined.update((a.asname or a.name).split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            defined.update(a.asname or a.name for a in node.names)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            defined.add(node.target.id)
        elif isinstance(node, ast.Assign):
            defined.update(t.id for t in node.targets if isinstance(t, ast.Name))
    annotations = []
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef):
            annotations.append(node.returns)
            args = node.args
            annotations += [a.annotation for a in args.posonlyargs + args.args + args.kwonlyargs]
            annotations += [a.annotation for a in (args.vararg, args.kwarg) if a]
        elif isinstance(node, ast.AnnAssign):
            annotations.append(node.annotation)
    used = {
        n.id
        for annotation in annotations
        if annotation is not None
        for n in ast.walk(annotation)
        if isinstance(n, ast.Name)
    }
    # Quoted annotations are strings in the AST: parse them too.
    for annotation in annotations:
        for n in ast.walk(annotation) if annotation is not None else ():
            if isinstance(n, ast.Constant) and isinstance(n.value, str):
                try:
                    used |= {m.id for m in ast.walk(ast.parse(n.value, mode="eval")) if isinstance(m, ast.Name)}
                except SyntaxError:
                    pass
    assert not used - defined, used - defined
