"""Adapt the pinned Flathub recipe without changing application identity."""
import argparse
import json
from pathlib import Path
import re

import yaml

UPSTREAM = '9720ef5bfbfd53e6809425a201ff5cdea93e98bf'
OPENAXIS = 'acc4da095cde6747556245b4b6c110c16b968b6b'


def adapt(recipe, sha):
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise ValueError('A full source commit is required')
    # Single-file downloads must include translations, not require a second bundle.
    recipe['separate-locales'] = False
    recipe['build-options']['env'] = {'CMAKE_BUILD_PARALLEL_LEVEL': '2', 'CTEST_PARALLEL_LEVEL': '2'}
    for module in recipe['modules']:
        if module['name'] not in ('PrusaSlicerDeps', 'PrusaSlicer'):
            continue
        source = module['sources'][0]
        assert source['url'] == 'https://github.com/prusa3d/PrusaSlicer'
        assert source.pop('tag') == 'version_2.9.6'
        source.update(url='https://github.com/rotatrix/PrusaSlicer.git', commit=sha)
        if module['name'] == 'PrusaSlicerDeps':
            module['build-commands'][0] = module['build-commands'][0].replace(
                '-DDEP_WX_GTK3=1', '-DDEP_WX_GTK3=1 -DDEP_MAX_THREADS=2')
            continue
        module['sources'].append(dict(type='git', url='https://github.com/rotatrix/openaxis.git',
                                      commit=OPENAXIS, dest='openaxis-sdk'))
        commands = module['build-commands']
        commands[0] = commands[0].replace('mkdir -p build && cd build',
            'git -C openaxis-sdk apply --ignore-space-change ../cmake/openaxis-openssl-compat.patch\n'
            'mkdir -p build && cd build')
        commands[0] = commands[0].replace('-DSLIC3R_PCH=OFF',
            '-DSLIC3R_OPENAXIS=ON -DOPENAXIS_SOURCE_DIR="$PWD/../openaxis-sdk" '
            '-DSLIC3R_ENABLE_FORMAT_STEP=ON -DSLIC3R_PCH=OFF')
        commands.append('/app/cmake/bin/cmake -S tests/openaxis -B build/openaxis-tests -G Ninja\n'
                        '/app/cmake/bin/cmake --build build/openaxis-tests\n'
                        'ctest --test-dir build/openaxis-tests --output-on-failure')
        commands.append('install -Dm644 LICENSE /app/share/licenses/PrusaSlicer/LICENSE\n'
                        'install -Dm644 openaxis-sdk/LICENSE /app/share/licenses/OpenAxis/LICENSE\n'
                        f'printf "%s\\n" "{sha}" > /app/share/PrusaSlicer/SOURCE-REVISION.txt')
        for item in module['sources']:
            if item['type'] == 'shell':
                item['commands'] = [c.replace('+flathub.org', '+Rotatrix') for c in item['commands']]
    return recipe


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('recipe', type=Path)
    parser.add_argument('sha')
    args = parser.parse_args()
    recipe = yaml.safe_load(args.recipe.read_text(encoding='utf-8'))
    args.recipe.with_suffix('.json').write_text(json.dumps(adapt(recipe, args.sha), indent=2) + '\n', encoding='utf-8')
