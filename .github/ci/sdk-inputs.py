"""Prepare immutable Base/cache inputs once; restore them with shared infrastructure.

Downstream source builds may invoke this entry point from an exact CMeta ref,
passing their own --consumer root. No product configuration downloads an SDK.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('phase', choices=('prepare', 'restore'))
parser.add_argument('--consumer', type=Path, default=Path.cwd())
parser.add_argument('--profile')
parser.add_argument('--prefix', default='')
args = parser.parse_args()
consumer = args.consumer.resolve()
inputs = consumer / 'build/ci-inputs'
infra = Path(os.environ['VCPKG_CACHE_REPOSITORY_ROOT']).resolve()
reader = infra / 'scripts/restore-native-dependency.py'
for key in ('SALTS_BASE_VERSION', 'VCPKG_CACHE_VERSION'):
    if not re.fullmatch(r'\d+\.\d+\.\d+', os.environ[key]):
        raise ValueError(key + ' must select an exact stable release')


def run(*command):
    subprocess.run(list(map(str, command)), check=True)


def api(repo, path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/{path}']))


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def asset(release, name):
    found = [a for a in release['assets'] if a['name'] == name]
    if len(found) != 1 or not re.fullmatch(r'sha256:[0-9a-f]{64}', found[0]['digest']):
        raise ValueError('Missing unique SHA-256-qualified release asset: ' + name)
    return found[0]


def release(repo, version):
    result = api(repo, 'releases/tags/v' + version)
    if result['draft'] or result['prerelease'] or result['tag_name'] != 'v' + version:
        raise ValueError('Expected a selected published stable release')
    return result


def environment(values):
    with Path(os.environ['GITHUB_ENV']).open('a', encoding='utf-8') as output:
        for key, value in values.items():
            if '\n' in str(value) or '\r' in str(value):
                raise ValueError('Invalid CI environment value')
            output.write(args.prefix + key + '=' + str(value).replace('\\', '/') + '\n')


if args.phase == 'prepare':
    inputs.mkdir(parents=True)
    (inputs / 'locks').mkdir()
    (inputs / 'blobs').mkdir()
    spec = importlib.util.spec_from_file_location('archive_transport', infra / 'scripts/setup-compiler-toolchain.py')
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)

    def acquire(value):
        path = inputs / 'blobs' / value['digest'][7:]
        transport.verified_download(value['browser_download_url'], path, value['digest'][7:], value['size'])
        return path

    selected = release('qigao/salts-base', os.environ['SALTS_BASE_VERSION'])
    metadata = asset(selected, 'salts-base-' + os.environ['SALTS_BASE_VERSION'] + '-manifest.json')
    manifest = read(acquire(metadata))
    if (manifest['schemaVersion'] != 1 or manifest['name'] != 'SaltsBase'
            or manifest['repository'] != 'https://github.com/qigao/salts-base'
            or manifest['version'] != os.environ['SALTS_BASE_VERSION']
            or manifest['tag'] != selected['tag_name'] or manifest['ci']['conclusion'] != 'success'):
        raise ValueError('Unqualified Base release manifest')
    write(inputs / 'base-release.json', {'release': selected, 'manifest': metadata})
    write(inputs / 'base-manifest.json', manifest)
    matrix = read(Path(__file__).with_name('sdk-matrix.json'))
    for profile in {p['sdk'] for p in matrix['include']}:
        sdks = [s for s in manifest['sdks'] if s['platform'] == profile]
        if len(sdks) != 1:
            raise ValueError('Base has no SDK for ' + profile)
        sdk = sdks[0]
        value = asset(selected, sdk['file'])
        if value['size'] != sdk['sizeBytes'] or value['digest'] != 'sha256:' + sdk['sha256']:
            raise ValueError('SDK manifest differs from release assets')
        acquire(value)
    for triplet in {p['triplet'] for p in matrix['include']}:
        lock = inputs / 'locks' / (triplet + '.json')
        run('python', reader, 'resolve-release', consumer / 'cmake/dependencies' / (triplet + '.lock.json'), '--output-lock', lock)
        run('python', reader, 'download', lock, '--destination', inputs / 'blobs')
    cache = release('qigao/vcpkg-cache', os.environ['VCPKG_CACHE_VERSION'])
    for name in ('re2c', 'lemon'):
        lock = read(consumer / 'cmake' / (name + '.lock.json'))
        name_on_release = lock['source']['id'] + '.' + lock['source']['version'] + '.nupkg'
        value = asset(cache, name_on_release)
        if value['digest'] != 'sha256:' + lock['sha256']:
            raise ValueError('Host tool release archive differs from package lock')
        acquire(value)
    compiler = read(infra / 'compiler-contracts/release-obfuscation.json')['sources']['llvm-mingw']
    compiler_asset = next(a for a in compiler['assets'] if a['host'] == 'windows-x64')
    lock = {'schema': 1, 'source': {'kind': 'url', 'url':
        f"https://github.com/{compiler['repository']}/releases/download/{compiler['release_tag']}/{compiler_asset['name']}"},
        'sha256': compiler_asset['sha256']}
    write(inputs / 'locks/windows-compiler.json', lock)
    run('python', reader, 'download', inputs / 'locks/windows-compiler.json', '--destination', inputs / 'blobs')
    with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as output:
        output.write('matrix=' + json.dumps(matrix, separators=(',', ':')) + '\n')
else:
    record = read(inputs / 'base-release.json')
    metadata = record['manifest']
    # Verify the original signed-by-release-API bytes, then use the parsed copy.
    original = inputs / 'blobs' / metadata['digest'][7:]
    if original.stat().st_size != metadata['size'] or 'sha256:' + digest(original) != metadata['digest']:
        raise ValueError('Prepared Base manifest digest mismatch')
    manifest = read(original)
    if manifest['version'] != os.environ['SALTS_BASE_VERSION']:
        raise ValueError('Prepared Base version differs from the selected environment')
    profiles = read(Path(__file__).with_name('sdk-matrix.json'))['include']
    profile = next(p for p in profiles if p['sdk'] == args.profile)
    sdk = next(s for s in manifest['sdks'] if s['platform'] == args.profile)
    archive = inputs / 'blobs' / sdk['sha256']
    if archive.stat().st_size != sdk['sizeBytes'] or digest(archive) != sdk['sha256']:
        raise ValueError('Prepared Base SDK digest mismatch')
    root = consumer / 'build/dependencies' / (args.prefix.lower() + 'base-' + args.profile)
    root.mkdir(parents=True)
    with tarfile.open(archive) as package:
        package.extractall(root, filter='data')
    write(root / 'sdk-restore-receipt.json', {'version': manifest['version'],
        'profile': args.profile, 'source': manifest['commit'],
        'producer': manifest['ci']['runId'], 'sha256': sdk['sha256']})
    destination = consumer / 'build/dependencies' / (args.prefix.lower() + 'vcpkg-' + args.profile)
    run('python', reader, 'locked', inputs / 'locks' / (profile['triplet'] + '.json'),
        '--destination', destination, '--payload', 'vcpkg_installed')
    # The shared restore receipt defines the payload root.
    receipt = read(destination / 'receipt.json')
    values = {'SALTS_BASE_ROOT': root, 'VCPKG_INSTALLED_DIR': receipt['root']}
    for name in ('re2c', 'lemon'):
        lock = read(consumer / 'cmake' / (name + '.lock.json'))
        values[name.upper() + '_ARCHIVE_FILE'] = inputs / 'blobs' / lock['sha256']
    environment(values)
    print(json.dumps({'profile': args.profile, 'baseSource': manifest['commit'], 'producer': manifest['ci']['runId']}))
