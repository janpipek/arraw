# /// script
# requires-python = ">=3.11"
# ///
"""Check sandbox mount boundaries without invoking Git or Podman."""

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import sandbox


class MountBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.workspace = self.root / "checkout"
        self.store = self.root / "store"
        self.photos = self.root / "photos"
        self.keep = self.root / "writes"
        for directory in (self.workspace / ".git", self.photos):
            directory.mkdir(parents=True)
        for replacement in (patch.object(sandbox, "REPO", self.workspace),
                            patch.object(sandbox, "store_root", return_value=self.store)):
            replacement.start()
            self.addCleanup(replacement.stop)

    def validate(self, photos=None, keep=None):
        return sandbox.validate_paths(str(photos) if photos else None,
                                      str(keep) if keep else None)

    def test_current_clone_is_accepted_without_creating_store_or_overlay(self):
        self.assertEqual(self.validate(photos=self.photos, keep=self.keep),
                         (self.workspace, self.store))
        self.assertFalse(self.store.exists())
        self.assertFalse(self.keep.exists())

    def test_worktree_and_symlinked_git_directory_are_refused(self):
        git_dir = self.workspace / ".git"
        git_dir.rmdir()
        git_dir.write_text(f"gitdir: {self.root / 'external-git'}\n")
        with self.assertRaisesRegex(SystemExit, "standalone clone"):
            self.validate()
        git_dir.unlink()
        git_dir.symlink_to(self.photos, target_is_directory=True)
        with self.assertRaisesRegex(SystemExit, "standalone clone"):
            self.validate()

    def test_shared_git_directory_is_refused(self):
        (self.workspace / ".git" / "commondir").write_text("../../external-git\n")
        with self.assertRaisesRegex(SystemExit, "standalone clone"):
            self.validate()

    def test_store_cannot_overlap_checkout(self):
        for store in (self.workspace / "store", self.root):
            with self.subTest(store=store), patch.object(sandbox, "store_root", return_value=store):
                with self.assertRaises(SystemExit):
                    self.validate()

    def test_store_symlink_cannot_expose_an_external_directory(self):
        self.store.mkdir()
        (self.store / "codex").symlink_to(self.photos, target_is_directory=True)
        with self.assertRaisesRegex(SystemExit, "store entry escapes"):
            self.validate()

    def test_photos_cannot_alias_writable_mounts(self):
        fixtures = self.workspace / "fixtures"
        fixtures.mkdir()
        self.store.mkdir()
        alias = self.root / "photo-alias"
        alias.symlink_to(fixtures, target_is_directory=True)
        for photos in (fixtures, alias, self.workspace, self.root, self.store):
            with self.subTest(photos=photos), self.assertRaises(SystemExit):
                self.validate(photos=photos)

    def test_overlay_output_cannot_overlap_photos_or_checkout(self):
        for keep in (self.photos, self.photos / "writes", self.root,
                     self.workspace / "writes", self.store):
            with self.subTest(keep=keep), self.assertRaises(SystemExit):
                self.validate(photos=self.photos, keep=keep)

    def test_overlay_symlinks_cannot_expose_originals(self):
        self.keep.mkdir()
        for name in ("changes", ".work"):
            alias = self.keep / name
            alias.symlink_to(self.photos, target_is_directory=True)
            with self.subTest(name=name), self.assertRaisesRegex(SystemExit, "must not be a symlink"):
                self.validate(photos=self.photos, keep=self.keep)
            alias.unlink()

    def test_mount_delimiters_are_refused(self):
        for name in ("path:options", "path,options", "path\noptions", "path\roptions"):
            with self.subTest(name=name), self.assertRaises(SystemExit):
                sandbox.host_path(self.root / name)

    def test_invalid_photo_path_fails_before_build_or_store_creation(self):
        args = argparse.Namespace(photos=str(self.workspace), keep_writes=None, dry_run=False)
        with patch.object(sandbox, "ensure_image") as build, patch.object(sandbox, "prepare_store") as store:
            with self.assertRaises(SystemExit):
                sandbox.run("podman", args)
            build.assert_not_called()
            store.assert_not_called()

    def test_launch_uses_current_clone_without_a_repo_argument(self):
        args = argparse.Namespace(photos=None, keep_writes=None,
                                  dry_run=False, gpu=False, gui=False, command=["true"],
                                  agent=None, safe=False)
        calls = []

        def subprocess_result(command, **kwargs):
            calls.append(command)
            return argparse.Namespace(returncode=0, stdout="Example\n")

        with patch.object(sandbox, "ensure_image"), patch.object(sandbox, "memory_limit", return_value="1g"), \
                patch.object(sandbox.subprocess, "run", side_effect=subprocess_result), \
                patch.object(sandbox.sys.stdin, "isatty", return_value=False):
            self.assertEqual(sandbox.run("podman", args), 0)
        self.assertEqual(calls[0][:3], ["git", "-C", str(self.workspace)])
        self.assertEqual(calls[1][:3], ["git", "-C", str(self.workspace)])
        command = calls[2]
        mounts = [command[index + 1] for index, argument in enumerate(command) if argument == "-v"]
        self.assertIn(f"{self.workspace}:/workspace:z", mounts)
        self.assertFalse(any("/.git" in mount for mount in mounts))

    def test_redirected_command_output_does_not_allocate_a_terminal(self):
        args = argparse.Namespace(photos=None, keep_writes=None,
                                  dry_run=False, gpu=False, gui=False,
                                  command=["git", "log", "--oneline"], agent=None, safe=False)
        with patch.object(sandbox, "ensure_image"), patch.object(sandbox, "git_identity", return_value=[]), \
                patch.object(sandbox, "memory_limit", return_value="1g"), \
                patch.object(sandbox.sys.stdin, "isatty", return_value=True), \
                patch.object(sandbox.sys.stdout, "isatty", return_value=False), \
                patch.object(sandbox.subprocess, "run", return_value=argparse.Namespace(returncode=0)) as run:
            sandbox.run("podman", args)
        command = run.call_args.args[0]
        self.assertNotIn("-t", command)
        self.assertNotIn("-it", command)

    def test_arguments_after_separator_go_to_the_named_agent(self):
        def launched(*argv):
            with patch.object(sandbox.sys, "argv", ["sandbox.py", *argv]), \
                    patch.object(sandbox, "podman", return_value="podman"), \
                    patch.object(sandbox, "ensure_image"), patch.object(sandbox, "git_identity", return_value=[]), \
                    patch.object(sandbox, "memory_limit", return_value="1g"), \
                    patch.object(sandbox.subprocess, "run", return_value=argparse.Namespace(returncode=0)) as run:
                sandbox.main()
            command = run.call_args.args[0]
            return command[command.index(sandbox.IMAGE) + 1:]

        self.assertEqual(launched("--safe", "claude", "--", "--resume"), ["claude", "--resume"])
        self.assertEqual(launched("claude", "--", "--resume"),
                         ["claude", "--dangerously-skip-permissions", "--resume"])
        self.assertEqual(launched("--", "just", "test"), ["just", "test"])

    def test_image_build_logs_do_not_pollute_command_output(self):
        with patch.object(sandbox, "inputs_hash", return_value="hash"), \
                patch.object(sandbox.subprocess, "run", return_value=argparse.Namespace(returncode=0)) as run:
            sandbox.build("podman", refresh=False)
        self.assertIs(run.call_args.kwargs["stdout"], sandbox.sys.stderr)


if __name__ == "__main__":
    unittest.main()
