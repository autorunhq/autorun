#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location('mesa_sdk', Path(__file__).resolve().parents[1] / 'tools/mesa_sdk.py')
SDK = importlib.util.module_from_spec(spec)
spec.loader.exec_module(SDK)


class MountedPosixPath(PurePosixPath):
    def is_dir(self):
        return True


class MesaSDKTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mesa-sdk-test-')
        self.root = Path(self.temporary.name)
        self.lib = self.root / 'sdk/lib'
        self.lib.mkdir(parents=True)
        self.path = self.root / 'sdk/share/mesa-switch/manifest.json'
        self.path.parent.mkdir(parents=True)
        self.legacy = self.root / 'source-revision.txt'
        self.legacy.write_text('a' * 40 + '\n')
        self.manifest = {'git_revision': 'b' * 40, 'source_dirty': False}
        self.save()

    def tearDown(self):
        self.temporary.cleanup()

    def save(self):
        self.path.write_text(json.dumps(self.manifest))

    def revision(self):
        return SDK.mesa_revision(self.lib, self.legacy)

    def test_selected_sdk_revision(self):
        self.assertEqual(self.revision(), 'b' * 40)

    def test_unidentified_sdk_does_not_use_another_builds_revision(self):
        self.path.unlink()
        self.assertIsNone(self.revision())

    def test_source_build_revision(self):
        lib = self.root / 'install/opt/devkitpro/portlibs/switch/lib'
        lib.mkdir(parents=True)
        self.assertEqual(SDK.mesa_revision(lib, self.legacy), 'a' * 40)
        self.legacy.write_text('a' * 40 + '-dirty')
        self.assertEqual(SDK.mesa_revision(lib, self.legacy), 'a' * 40 + '-dirty')
        self.legacy.unlink()
        self.assertIsNone(SDK.mesa_revision(lib, self.legacy))

    def test_dirty_sdk_is_recorded(self):
        self.manifest['source_dirty'] = True
        self.save()
        self.assertEqual(self.revision(), 'b' * 40 + '-dirty')

    def test_missing_configured_directory_rejected(self):
        with self.assertRaisesRegex(ValueError, 'configured'):
            SDK.mesa_revision(self.lib / 'missing', self.legacy)

    def test_invalid_revision_rejected(self):
        for revision in ('short', None, 123):
            with self.subTest(revision=revision):
                self.manifest['git_revision'] = revision
                self.save()
                with self.assertRaisesRegex(ValueError, 'revision'):
                    self.revision()

    def test_invalid_manifest_rejected(self):
        for contents in ('{', '[]'):
            with self.subTest(contents=contents):
                self.path.write_text(contents)
                with self.assertRaises(ValueError):
                    self.revision()

    def test_path_mapping_linux(self):
        with mock.patch.object(SDK.os, 'name', 'posix'), mock.patch.object(SDK, 'Path', MountedPosixPath):
            for path in ('/c/Users/First Last/sdk/lib', 'C:/Users/First Last/sdk/lib',
                         'C:\\Users\\First Last\\sdk\\lib', '/mnt/c/Users/First Last/sdk/lib'):
                self.assertEqual(SDK.host_path(path), PurePosixPath('/mnt/c/Users/First Last/sdk/lib'))
            self.assertEqual(SDK.host_path('/opt/devkitpro/portlibs/switch/lib'),
                             PurePosixPath('/opt/devkitpro/portlibs/switch/lib'))

    def test_native_linux_single_letter_directory(self):
        with mock.patch.object(SDK.os, 'name', 'posix'), mock.patch.object(SDK, 'Path', MountedPosixPath):
            with mock.patch.object(MountedPosixPath, 'is_dir', return_value=False):
                self.assertEqual(SDK.host_path('/a/native/lib'), PurePosixPath('/a/native/lib'))

    def test_path_mapping_windows(self):
        with mock.patch.object(SDK.os, 'name', 'nt'), mock.patch.object(SDK, 'Path', PureWindowsPath):
            for path in ('/c/Users/First Last/sdk/lib', 'C:/Users/First Last/sdk/lib',
                         'C:\\Users\\First Last\\sdk\\lib', '/mnt/c/Users/First Last/sdk/lib'):
                self.assertEqual(SDK.host_path(path), PureWindowsPath('C:/Users/First Last/sdk/lib'))


if __name__ == '__main__':
    unittest.main()
