#!/usr/bin/env python3
"""Use pinned VVL's dependency builder, fetching only each known-good revision."""
import importlib.util
from pathlib import Path
import subprocess
import sys

source = Path(sys.argv.pop(1)).resolve(strict=True)
spec = importlib.util.spec_from_file_location('vvl_update_deps', source / 'scripts/update_deps.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def checkout(repo):
    if repo._args.do_clean_repo or repo._args.ref:
        raise RuntimeError('Only fresh pinned dependency builds are supported')
    path = Path(repo.repo_dir)
    if path.exists():
        raise RuntimeError('Refusing to replace an existing dependency: ' + str(path))
    path.mkdir(parents=True)
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=path, text=True).strip()
    git('init', '--quiet')
    git('remote', 'add', 'origin', repo.url)
    git('fetch', '--depth=1', '--no-tags', 'origin', repo.commit)
    revision = git('rev-parse', 'FETCH_HEAD')
    git('checkout', '--detach', revision)
    if git('rev-parse', '--is-shallow-repository') != 'true':
        raise RuntimeError('Dependency is not shallow: ' + repo.name)
    print(f'PINNED_DEPENDENCY {repo.name} requested={repo.commit} resolved={revision}', flush=True)


module.GoodRepo.Checkout = checkout
module.main()
