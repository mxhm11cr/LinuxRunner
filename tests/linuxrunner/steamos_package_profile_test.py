#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('profile', Path(__file__).resolve().parents[2] / 'scripts/steamos-package-profile.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

class ProfileTests(unittest.TestCase):
    def package(self, database, name, files, dependencies=(), provides=(), size=1):
        directory = database / (name + '-1-1'); directory.mkdir()
        values = dict(NAME=[name], VERSION=['1-1'], ARCH=['aarch64'], DEPENDS=dependencies, PROVIDES=provides)
        if size is not None: values['SIZE'] = [str(size)]
        (directory / 'desc').write_text(''.join('%' + key + '%\n' + '\n'.join(value) + '\n\n' for key, value in values.items()))
        (directory / 'files').write_text('%FILES%\n' + '\n'.join(files) + '\n')

    def test_whole_packages_and_real_database_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp)
            self.package(db, 'steam', ['usr/bin/steam', 'usr/share/steam/config'], ['lib>=1'], size=12)
            self.package(db, 'lib', ['usr/lib/libtest.so'], size=4)
            result, patterns = module.plan(db, ['steam'], {})
            self.assertEqual(result['packages'], ['lib', 'steam']); self.assertEqual(result['installed_bytes'], 16)
            for path in ['/usr', '/usr/bin/steam', '/usr/lib/libtest.so', '/usr/lib/holo/pacmandb/local/steam-1-1/desc']:
                self.assertTrue(any(re.fullmatch(p, path) for p in patterns), path)
            self.assertFalse(any(re.fullmatch(p, '/usr/bin/not-steam') for p in patterns))
            self.assertFalse(result['dependency_constraints_verified']); self.assertFalse(result['steam_client_verified'])
            self.assertEqual(result['dependencies'], ['lib>=1'])

    def test_provider_must_be_explicit_when_ambiguous(self):
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp); self.package(db,'app',['usr/bin/app'],['font'])
            self.package(db,'font-a',['usr/share/a'],provides=['font']); self.package(db,'font-b',['usr/share/b'],provides=['font'])
            with self.assertRaises(ValueError): module.plan(db,['app'],{})
            result,_=module.plan(db,['app'],{'font':'font-b'}); self.assertEqual(result['packages'],['app','font-b'])
            with self.assertRaises(ValueError): module.plan(db,['app'],{'font':'app'})

    def test_exact_soname_version_provider(self):
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp); self.package(db,'app',['usr/bin/app'],['libcrypt.so=2-64'])
            self.package(db,'v1',['usr/lib/libcrypt.so.1'],provides=['libcrypt.so=1-64'])
            self.package(db,'v2',['usr/lib/libcrypt.so.2'],provides=['libcrypt.so=2-64'])
            result,_=module.plan(db,['app'],{});self.assertEqual(result['packages'],['app','v2'])

    def test_missing_dependency_is_not_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp);self.package(db,'app',['usr/bin/app'],['missing'])
            with self.assertRaises(ValueError):module.plan(db,['app'],{})

    def test_unsafe_owned_paths_rejected(self):
        for path in ['../escape','/outside','a/../b','a//b','a/./b','a\0b','a\nb']:
            with self.subTest(path=path), self.assertRaises(ValueError):module.restore_pattern([path])

    def test_literal_metacharacters_and_unicode(self):
        path='usr/share/ž/+name[1].(data)$'
        p=module.restore_pattern([path]);self.assertTrue(re.fullmatch(p,'/'+path))
        self.assertFalse(re.fullmatch(p,'/usr/share/ž/name1Xdata'))

    def test_batched_filters_retain_all_paths(self):
        paths=['usr/lib/app/' + str(n) + '.so' for n in range(100)]
        filters=module.restore_patterns(paths,limit=150)
        self.assertGreater(len(filters),1);self.assertTrue(all(len(p.encode())<=150 for p in filters))
        for path in paths:self.assertTrue(any(re.fullmatch(p,'/'+path) for p in filters))

    def test_zero_size_metapackage(self):
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp);self.package(db,'meta',[],size=None)
            result,_=module.plan(db,['meta'],{});self.assertEqual(result['installed_bytes'],0)

    def test_non_arm64_metadata_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp);self.package(db,'foreign',['usr/bin/foreign'])
            p=db/'foreign-1-1/desc';p.write_text(p.read_text().replace('aarch64','x86_64'))
            with self.assertRaises(ValueError):module.plan(db,['foreign'],{})

if __name__=='__main__': unittest.main()
