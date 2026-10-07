#!/usr/bin/env python3
"""macOS launcher using the shared settings and translation model."""
import argparse
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading

import bbport_launcher_win as shared

OUTPUTS = [*shared.OUTPUTS, ('3456x1944', ('3456 × 1944 (MacBook panel fit)',))]

PORT = Path(__file__).resolve().parent.parent
PACKAGED = os.environ.get('BB_PACKAGED') == '1'
DATA = Path(os.environ.get('BB_DATA_DIR',
    str(Path.home() / 'Library/Application Support/bbport') if PACKAGED else str(PORT)))
DEPS = Path(os.environ.get('BB_DEPS_PREFIX', Path.home() / 'bbport-deps/x86_64'))
SETTINGS = DATA / 'launcher-settings.json'
shared.PORT_DIR = PORT
shared.DATA_DIR = DATA
shared.CONFIG_FILE = SETTINGS
shared.INI_DEFAULTS = {**shared.INI_DEFAULTS, 'upscaler': 'fsr3'}
DEFAULTS = {**shared.APP_DEFAULTS, 'game_dir': os.environ.get('BB_GAME_DIR', ''),
            'ui_language': 'en', 'check_updates': False}


def load_settings():
    values = json.loads(SETTINGS.read_text()) if SETTINGS.exists() else {}
    if not isinstance(values, dict):
        raise ValueError(f'Launcher settings must be an object: {SETTINGS}')
    return {**DEFAULTS, **values}


def environment(settings):
    env = shared.game_environment(settings)
    env.update(BB_DATA_DIR=str(DATA), BB_DEPS_PREFIX=str(DEPS), PYTHON=sys.executable)
    if PACKAGED:
        env.update(BB_PACKAGED='1', BB_PREBUILT='1')
    return env


def command(arguments=()):
    return ['/bin/bash', str(PORT / 'run.sh'), *arguments]


class Launcher:
    def __init__(self, root, tk, ttk, dialogs):
        self.root, self.tk, self.ttk, self.dialogs = root, tk, ttk, dialogs
        self.settings = load_settings()
        shared.LANG = self.settings['ui_language']
        if shared.ini_path().exists():
            shared.ini_path().read_text(encoding='utf-8')
        self.ini, self.ini_lines = shared.load_ini()
        self.process = None
        self.events = queue.Queue()
        self.variables = {}
        root.title('Bloodborne — bbport for macOS')
        root.geometry('800x620')
        frame = ttk.Frame(root, padding=16)
        frame.pack(fill='both', expand=True)
        self.field(frame, 'game_dir', 'Game folder', self.settings, folder=True)
        self.field(frame, 'user_dir', 'Saves folder', self.settings, folder=True)
        self.choice(frame, 'ui_language', 'Launcher language', self.settings,
                    shared.UI_LANGUAGES)
        self.choice(frame, 'fps_mode', 'Frame rate', self.settings, shared.FPS_MODES)
        self.choice(frame, 'upscaler', 'Upscaler', self.ini,
                    [entry for entry in shared.UPSCALERS if entry[0] in ('fsr3', 'taa', 'off')])
        self.choice(frame, 'preset', 'Preset', self.ini, shared.PRESETS)
        self.choice(frame, 'output_res', 'Output resolution', self.ini, OUTPUTS)
        self.choice(frame, 'language', 'Game language', self.settings, shared.LANGUAGES)
        self.field(frame, 'player_name', 'Player name', self.settings)
        self.fullscreen = tk.BooleanVar(value=self.settings['fullscreen'])
        ttk.Checkbutton(frame, text=shared._('Fullscreen'), variable=self.fullscreen).pack(anchor='w')
        actions = ttk.Frame(frame)
        actions.pack(fill='x', pady=12)
        self.start_button = ttk.Button(actions, text=shared._('Start'), command=self.start)
        self.start_button.pack(side='left')
        self.stop_button = ttk.Button(actions, text=shared._('Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='left', padx=8)
        ttk.Button(actions, text=shared._('Save'), command=self.save).pack(side='left')
        ttk.Button(actions, text=shared._('Port folder'), command=self.open_data).pack(side='right')
        self.log = tk.Text(frame, height=12, wrap='word', state='disabled')
        self.log.pack(fill='both', expand=True)
        self.root.after(100, self.drain)
        root.protocol('WM_DELETE_WINDOW', self.close)

    def field(self, frame, key, label, values, folder=False):
        row = self.ttk.Frame(frame)
        row.pack(fill='x', pady=3)
        self.ttk.Label(row, text=shared._(label), width=22).pack(side='left')
        variable = self.tk.StringVar(value=values[key])
        self.variables[key] = (variable, values, None)
        self.ttk.Entry(row, textvariable=variable).pack(side='left', fill='x', expand=True)
        if folder:
            self.ttk.Button(row, text=shared._('Browse'),
                command=lambda: self.browse(variable)).pack(side='left', padx=(8, 0))

    def choice(self, frame, key, label, values, choices):
        row = self.ttk.Frame(frame)
        row.pack(fill='x', pady=3)
        self.ttk.Label(row, text=shared._(label), width=22).pack(side='left')
        mapping = {shared._(*text): value for value, text in choices}
        selected = next((text for text, value in mapping.items() if value == values[key]), None)
        if selected is None:
            raise ValueError(f'Unsupported macOS setting {key}={values[key]} in {shared.ini_path()}')
        variable = self.tk.StringVar(value=selected)
        self.variables[key] = (variable, values, mapping)
        self.ttk.Combobox(row, textvariable=variable, values=list(mapping), state='readonly').pack(
            side='left', fill='x', expand=True)

    def browse(self, variable):
        value = self.dialogs['folder'].askdirectory(parent=self.root, initialdir=variable.get() or str(Path.home()))
        if value:
            variable.set(value)

    def save(self):
        try:
            for key, (variable, values, mapping) in self.variables.items():
                values[key] = (
                    mapping[variable.get()] if mapping else variable.get())
            self.settings['fullscreen'] = self.fullscreen.get()
            DATA.mkdir(parents=True, exist_ok=True)
            SETTINGS.write_text(json.dumps(self.settings, indent=2, ensure_ascii=False) + '\n')
            shared.save_ini(self.ini, self.ini_lines)
            return True
        except (OSError, ValueError, KeyError) as error:
            self.dialogs['message'].showerror('Bloodborne', str(error), parent=self.root)
            return False

    def start(self):
        if self.process or not self.save():
            return
        game = Path(self.settings['game_dir'])
        if not (game / 'eboot.bin').is_file() or not (game / 'sce_module/libc.prx').is_file():
            self.dialogs['message'].showerror('Bloodborne',
                'Choose the complete game folder containing eboot.bin and sce_module/libc.prx.', parent=self.root)
            return
        try:
            self.process = subprocess.Popen(command(), cwd=PORT, env=environment(self.settings),
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True)
        except OSError as error:
            self.dialogs['message'].showerror('Bloodborne', str(error), parent=self.root)
            return
        self.start_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()

    def read_output(self, process):
        for line in iter(process.stdout.readline, b''):
            self.events.put(line.decode('utf-8', errors='replace'))
        self.events.put(process.wait())

    def drain(self):
        try:
            while True:
                event = self.events.get_nowait()
                if isinstance(event, int):
                    self.process = None
                    self.start_button.configure(state='normal')
                    self.stop_button.configure(state='disabled')
                    event = f'Game exited with status {event}\n'
                self.log.configure(state='normal')
                self.log.insert('end', event)
                self.log.see('end')
                self.log.configure(state='disabled')
        except queue.Empty:
            pass
        self.root.after(100, self.drain)

    def stop(self):
        if self.process and self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def open_data(self):
        DATA.mkdir(parents=True, exist_ok=True)
        subprocess.run(['/usr/bin/open', str(DATA)], check=True)

    def close(self):
        if self.save():
            self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--play', action='store_true')
    args, probe_arguments = parser.parse_known_args()
    if sys.platform != 'darwin':
        raise SystemExit('This launcher requires macOS.')
    if args.play:
        return subprocess.call(command(probe_arguments), cwd=PORT, env=environment(load_settings()))
    if probe_arguments:
        parser.error('Probe arguments require --play.')
    try:
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox
    except ImportError as error:
        raise SystemExit('macOS launcher requires Python with Tk; use the bundled runtime or install python-tk.') from error
    root = tk.Tk()
    Launcher(root, tk, ttk, {'folder': filedialog, 'message': messagebox})
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
