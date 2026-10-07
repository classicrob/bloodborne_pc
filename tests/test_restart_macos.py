#!/usr/bin/env python3
"""Exercise actual restart/launcher; mock only the costly game preprocessing stages."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'out/mac-playability'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'test-restart'
subprocess.run(['clang', '-arch', 'x86_64', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I.', '-Isrc', 'tests/test_restart_macos.c', '-Wl,-dead_strip', '-o', str(exe)],
               cwd=root, check=True)
subprocess.run(['python3', 'packaging/macos/sign.py', str(exe)], cwd=root, check=True)
with tempfile.TemporaryDirectory(prefix='restart-', dir=out) as tmp:
    temp = Path(tmp).resolve()
    game = temp / 'game'
    game.mkdir()
    (game / 'eboot.bin').touch()
    python = temp / 'preprocess'
    python.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                      'if sys.argv[1].endswith("/mods.py"): print(os.path.realpath(sys.argv[2]))\n')
    python.chmod(0o755)
    for custom in (False, True):
        env = os.environ.copy()
        for key in ('BB_CONFIG', 'BB_USER_DIR', 'BB_RESTART_TEST_STAGE', 'BB_RESTART_TEST_PID'):
            env.pop(key, None)
        env.update(BB_PREBUILT='1', BB_PROBE=str(exe), BB_GAME_DIR=str(game), BB_DATA_DIR=str(temp),
                   PYTHON=str(python), BB_MODS_ENABLED='0', BB_FPS='30', BB_RENDER_RES='1920x1080',
                   BB_RESTART_EXPECT_USER=str(temp/'user'), BB_RESTART_EXPECT_CONFIG=str(temp/'bbport.ini'))
        if custom:
            env.update(BB_USER_DIR=str(temp/'custom saves'), BB_CONFIG=str(temp/'custom settings.ini'))
            env['BB_RESTART_EXPECT_USER'] = env['BB_USER_DIR']
            env['BB_RESTART_EXPECT_CONFIG'] = env['BB_CONFIG']
        subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
