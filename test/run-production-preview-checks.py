#!/usr/bin/env python3
"""Run real-engine forecast, Jev attack or Jev exit checks with local assets and no model calls."""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/windows-hotkeys')
    parser.add_argument('--assets-dir', type=Path, default=ROOT / 'runs/base')
    parser.add_argument('--suite', choices=['production-preview', 'jev-attack', 'jev-exits'], default='production-preview')
    parser.add_argument('--attack-candidate', action='store_true',
                        help='test the saved attack-approach experiment in a generated source copy')
    args = parser.parse_args()
    if args.run_only and args.build_only:
        parser.error('--run-only and --build-only are mutually exclusive')
    build = args.build_dir.resolve()
    output = ROOT / 'build' / args.suite
    hook, source_name = {
        'production-preview': ('RuntimeProductionPreviewChecks', 'production_preview_runtime.cpp'),
        'jev-attack': ('RuntimeJevAttackChecks', 'jev_attack_runtime.cpp'),
        'jev-exits': ('RuntimeJevExitsChecks', 'jev_exits_runtime.cpp'),
    }[args.suite]
    output.mkdir(parents=True, exist_ok=True)
    if not args.run_only:
        subprocess.run(['cmake', '--build', str(build), '--parallel', '6'], check=True)
        entries = json.loads((build / 'compile_commands.json').read_text())
        sources = {'main.cpp': ROOT / 'test' / source_name}
        if args.suite in ('jev-attack', 'jev-exits'):
            # Expose controller internals only in this generated test build. The game header stays unchanged.
            header = (ROOT / 'src/jevai.hpp').read_text()
            (output / 'jevai.hpp').write_text(header.replace('class JevAi final : public NoneAi {',
                                                            'class JevAi final : public NoneAi {\npublic:'))
            sources['jevai.cpp'] = output / 'jevai.cpp'
            sources['jevai.cpp'].write_text((ROOT / 'src/jevai.cpp').read_text())
            if args.attack_candidate:
                subprocess.run(['patch', '-t', '-p2', '-i', str(ROOT / 'tools/jev_experiments/attack-approach.patch')],
                               cwd=output, check=True)
        for name, before, after in [
            ('game_manager.cpp', '    GameManager_GameSetup(game_state);',
             f'    GameManager_GameSetup(game_state);\n    extern void {hook}();\n    {hook}();'),
            ('svga.cpp', 'Uint32 flags = Svga_SetupDisplayMode(&bounds);',
             'Uint32 flags = Svga_SetupDisplayMode(&bounds) | SDL_WINDOW_HIDDEN;'),
        ]:
            text = (ROOT / 'src' / name).read_text()
            assert text.count(before) == 1, name
            sources[name] = output / name
            sources[name].write_text(text.replace(before, after))
        replacements = {}
        for original, source in sources.items():
            entry = next(e for e in entries if Path(e['file']).name == original)
            command = entry.get('arguments') or shlex.split(entry['command'])
            command = list(command)
            command.insert(1, '-I' + str(output))
            obj = output / (original + '.obj')
            command[command.index('-c') + 1] = str(source)
            command[command.index('-o') + 1] = str(obj)
            subprocess.run(command, cwd=build, check=True)
            replacements[f'CMakeFiles/max.dir/src/{original}.obj'] = str(obj)
        commands = subprocess.check_output(['ninja', '-t', 'commands', 'max-port.exe'], cwd=build, text=True)
        command = shlex.split(next(line for line in reversed(commands.splitlines()) if ' -o max-port.exe ' in line))
        start = next(i for i, arg in enumerate(command) if Path(arg).name.endswith(('g++', 'clang++')))
        command = command[start:]
        if '&&' in command:
            command = command[:command.index('&&')]
        command = [replacements.get(arg, arg) for arg in command]
        command[command.index('-o') + 1] = str(output / 'checks.exe')
        subprocess.run(command, cwd=build, check=True)
    if args.build_only:
        return
    runtime = output / 'runtime'
    runtime.mkdir(exist_ok=True)
    for source in args.assets_dir.iterdir():
        if source.name in {'max-port.exe', 'settings.json', 'stdout.txt', 'console.log', '.busy', '.lock'}:
            continue
        dest = runtime / source.name
        if not dest.exists() and not dest.is_symlink():
            dest.symlink_to(source.resolve())
    shutil.copy2(args.assets_dir / 'settings.json', runtime / 'settings.json')
    shutil.copy2(output / 'checks.exe', runtime / 'checks.exe')
    with (runtime / 'console.log').open('w') as log:
        subprocess.run(['wine', 'checks.exe'], cwd=runtime, stdout=log, stderr=subprocess.STDOUT,
                       timeout=120, check=True)
    print((runtime / f'{args.suite}-checks.txt').read_text())


if __name__ == '__main__':
    main()
