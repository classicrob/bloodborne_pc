#!/usr/bin/env python3
"""Shader corpus sweep (Apple): every shader the game ships through the production recompiler.

Extracts the corpus (extract.py), runs out/gpu/shader-sweep over it (at most two translated
processes; a job that stops the process is recorded and the driver restarts after it), checks
every SPIR-V module with spirv-val, and writes under OUT:
  results.tsv     one line per job: hash, kind, status (PASS / LOUD / CRASH / HANG / SETUP),
                  message, spirv-val, logged errors, name
  report.md       counts, LOUD grouped by message, crashes, spirv-val failures, logged errors
  gs_census.md    every ES/GS/copy configuration the game ships, with the production
                  GsEmulationUnsupported verdict (static census)
Run it in a heavy slot:
  tools/macos/heavy.sh --pool build "shader sweep" -- python3 -I tools/macos/shader_sweep/sweep.py \\
      --game ~/CUSA03173 --gpu-build out/gpu --out out/shader_sweep
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIRST_OUTPUT_SECONDS = 60   # Rosetta rule: no output within 60 s is reported, not retried
JOB_SECONDS = 120
STAGE_NAMES = {'cs': 'compute', 'vs': 'vertex', 'ps': 'fragment', 'es': 'export (ES as compute)',
               'gs': 'geometry (GS as compute + copy VS)', 'hs': 'hull', 'ds': 'domain (VS after HS)',
               'ls': 'local (LS)'}


def read_jobs(path):
    jobs = []
    for line in open(path):
        fields = line.split()
        kv = collections.defaultdict(list)
        for field in fields[4:]:
            key, _, value = field.partition('=')
            kv[key].append(value)
        jobs.append({'hash': fields[0], 'kind': fields[1], 'kv': kv,
                     'name': kv['name'][0], 'files': kv['files'][0]})
    return jobs


class Worker:
    """Runs the driver over [first, end), restarting after each job that ends the process."""

    def __init__(self, args, env, jobs, results, lock, hull=None):
        self.args, self.env, self.jobs, self.results, self.lock = args, env, jobs, results, lock
        self.hull = hull
        self.hulls = []

    def run(self, first, end):
        while first < end:
            first = self.launch(first, end)

    def launch(self, first, end):
        command = [self.args.driver, self.args.jobs_file, str(first), str(end)]
        if self.hull is not None:
            command.append(str(self.hull))
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                   env=self.env, text=True, errors='replace')
        lines, current, started = [], None, False
        deadline = [time.monotonic() + FIRST_OUTPUT_SECONDS]
        timed_out = []

        def watchdog():
            while process.poll() is None:
                if time.monotonic() > deadline[0]:
                    timed_out.append(True)
                    process.kill()
                    return
                time.sleep(1)
        threading.Thread(target=watchdog, daemon=True).start()
        for line in process.stderr:
            line = line.rstrip('\n')
            deadline[0] = time.monotonic() + JOB_SECONDS
            if line.startswith('@@PROFILE'):
                started = True
                with self.lock:
                    self.results.setdefault('profile', line[10:])
            elif line.startswith('@@JOB '):
                current = int(line.split()[1])
                lines = []
            elif line.startswith('@@DONE '):
                self.finish(current, 'PASS', '', lines)
                current = None
            elif line.startswith('@@HULL '):
                self.hulls.append(int(line.split()[1]))
            elif line.startswith('@@GS ') or line.startswith('@@GAP '):
                lines.append(line)
            elif line.startswith('@@HULLJOB') or line.startswith('@@END'):
                pass
            else:
                lines.append(line)
        process.wait()
        if not started:
            raise SystemExit('shader-sweep: the driver %s before its first output (exit %s):\n%s' % (
                'hung' if timed_out else 'stopped', process.returncode, '\n'.join(lines[-20:])))
        if current is None:
            if process.returncode != 0:
                raise SystemExit(f'shader-sweep: driver exit {process.returncode} between jobs')
            return end
        code = process.returncode
        setup = [l[8:] for l in lines if l.startswith('@@SETUP ')]
        if timed_out:
            status, message = 'HANG', f'no output for {JOB_SECONDS} s'
        elif setup:
            status, message = 'SETUP', setup[0]
        elif code == 23:
            status, message = 'LOUD', loud_message(lines)
        elif code < 0:
            status, message = 'CRASH', signal.Signals(-code).name
        else:
            status, message = 'CRASH', f'exit {code}'
        self.finish(current, status, message, lines)
        return current + 1

    def finish(self, index, status, message, lines):
        errors = [strip_log(l) for l in lines
                  if '<Error>' in l or '<Critical>' in l or '<Warning>' in l]
        gs = {}
        for l in lines:
            if l.startswith('@@GS '):
                gs['in_prim'] = int(l.split()[3])
            elif l.startswith('@@GAP '):
                gs['gap'] = l.split(' ', 2)[2]
        if status == 'PASS' and 'gap' in gs:
            status, message = 'LOUD', ('macOS geometry shader emulation does not cover ' +
                                       gs['gap'] + ' (draw-time check)')
        with self.lock:
            self.results[index] = {'status': status, 'message': message, 'errors': errors,
                                   'gs': gs, 'log': lines[-40:]}


def strip_log(line):
    # "GPU [Class] <Level> file:line function: message" -> "<Level> message"
    m = re.match(r'GPU \[[^\]]*\] <(\w+)> \S+ [^:]*: (.*)', line)
    return f'{m.group(1)}: {m.group(2)}' if m else line


def loud_message(lines):
    """The production stop message: the Critical log line(s) of the stopped job."""
    out = []
    for i, line in enumerate(lines):
        if '<Critical>' in line:
            text = strip_log(line)
            text = re.sub(r'^Critical: ', '', text)
            # Assertion messages continue on the next line.
            if text.endswith('Assertion Failed!') and i + 1 < len(lines):
                text += ' ' + lines[i + 1].strip()
            out.append(text)
    return ' | '.join(out) if out else 'stopped (exit 23) without a Critical log line'


def normalize(message):
    return re.sub(r'0x[0-9a-fA-F]+|\b\d+\b', 'N', message)


def spirv_files(user_dir):
    files = collections.defaultdict(list)
    for directory, _, names in os.walk(user_dir):
        for name in names:
            if name.endswith('.spv'):
                m = re.search(r'0x([0-9a-f]{16})', name)
                if m:
                    files[int(m.group(1), 16)].append(os.path.join(directory, name))
    return files


def spirv_val(path):
    result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', path],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return None
    first = (result.stderr or result.stdout).strip().splitlines()
    return first[0] if first else f'exit {result.returncode}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', default=os.environ.get('BB_GAME_DIR'))
    parser.add_argument('--gpu-build', default='out/gpu')
    parser.add_argument('--out', default='out/shader_sweep')
    parser.add_argument('--processes', type=int, default=2)
    parser.add_argument('--only', help='comma-separated hashes (debugging)')
    args = parser.parse_args()
    if not args.game:
        raise SystemExit('shader-sweep: --game or BB_GAME_DIR')
    out = os.path.abspath(args.out)
    corpus = os.path.join(out, 'corpus')
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'extract.py'), args.game, corpus],
                   check=True)
    args.jobs_file = os.path.join(corpus, 'jobs.txt')
    args.driver = os.path.abspath(os.path.join(args.gpu_build, 'shader-sweep'))
    if not os.access(args.driver, os.X_OK):
        raise SystemExit(f'shader-sweep: no driver at {args.driver} (ninja -C out/gpu shader-sweep)')
    jobs = read_jobs(args.jobs_file)
    if args.only:
        wanted = {int(h, 16) for h in args.only.split(',')}
        keep = [i for i, j in enumerate(jobs) if int(j['hash'], 16) in wanted]
    else:
        keep = list(range(len(jobs)))
    user = os.path.join(out, 'user')
    shutil.rmtree(user, ignore_errors=True)
    os.makedirs(user)
    # The MoltenVK ICD the port uses (as build.sh sets it).
    deps = os.environ.get('BB_DEPS_PREFIX', os.path.expanduser('~/bbport-deps/x86_64'))
    icd = os.environ.get('VK_DRIVER_FILES', os.path.join(deps, 'share/vulkan/icd.d/MoltenVK_icd.json'))
    if not os.path.isfile(icd):
        raise SystemExit(f'shader-sweep: no MoltenVK ICD at {icd} (set BB_DEPS_PREFIX)')
    env = dict(os.environ, BB_GPU_USER_DIR=user, BB_DUMP_SHADERS='1', BB_PIPELINE_CACHE='0',
               BB_GPU_LOG='warning', VK_DRIVER_FILES=icd)
    results, lock = {}, threading.Lock()
    start = time.monotonic()

    def ranges(indices, parts):
        """Contiguous runs of `indices`, split into about `parts` per run."""
        runs, run = [], []
        for i in indices:
            if run and i != run[-1] + 1:
                runs.append(run)
                run = []
            run.append(i)
        if run:
            runs.append(run)
        out_ranges = []
        for run in runs:
            step = max(1, -(-len(run) // parts))
            out_ranges += [(run[k], run[min(k + step, len(run)) - 1] + 1) for k in range(0, len(run), step)]
        return out_ranges

    main_jobs = [i for i in keep if jobs[i]['kind'] != 'ls']
    ls_jobs = [i for i in keep if jobs[i]['kind'] == 'ls']
    workers = []
    with concurrent.futures.ThreadPoolExecutor(args.processes) as pool:
        futures = []
        for first, end in ranges(main_jobs, args.processes):
            worker = Worker(args, env, jobs, results, lock)
            workers.append(worker)
            futures.append(pool.submit(worker.run, first, end))
        for f in futures:
            f.result()
    if ls_jobs:
        hulls = sorted(h for w in workers for h in w.hulls)
        if not hulls:
            hs = next((i for i, j in enumerate(jobs) if j['kind'] == 'hs'), None)
            if hs is None:
                raise SystemExit('shader-sweep: LS jobs but no HS job')
            probe = Worker(args, env, jobs, {}, threading.Lock())
            probe.run(hs, hs + 1)
            hulls = probe.hulls
        if not hulls:
            for i in ls_jobs:
                results[i] = {'status': 'SETUP', 'message': 'no HS with a tess constant buffer compiled',
                              'errors': [], 'gs': {}, 'log': []}
        else:
            worker = Worker(args, env, jobs, results, lock, hull=hulls[0])
            for first, end in ranges(ls_jobs, 1):
                worker.run(first, end)
    compile_seconds = time.monotonic() - start

    # BB_DUMP_SHADERS also writes IR text per pass (gigabytes over the corpus); keep the
    # guest code and SPIR-V.
    for directory, _, names in os.walk(user):
        for name in names:
            if not name.endswith(('.spv', '.bin')):
                os.remove(os.path.join(directory, name))
    spv = spirv_files(user)
    by_hash = collections.defaultdict(list)
    for i in keep:
        by_hash[int(jobs[i]['hash'], 16)].append(i)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        verdicts = dict(zip([p for paths in spv.values() for p in paths],
                            pool.map(spirv_val, [p for paths in spv.values() for p in paths])))
    for shader_hash, indices in by_hash.items():
        failures = [f'{os.path.basename(p)}: {verdicts[p]}' for p in spv.get(shader_hash, [])
                    if verdicts[p]]
        for i in indices:
            r = results.setdefault(i, {'status': 'NOT RUN', 'message': '', 'errors': [], 'gs': {}, 'log': []})
            r['spirv'] = len(spv.get(shader_hash, []))
            r['spirv_failures'] = failures
    write_reports(out, jobs, keep, results, compile_seconds, time.monotonic() - start)


def write_reports(out, jobs, keep, results, compile_seconds, total_seconds):
    with open(os.path.join(out, 'results.tsv'), 'w') as f:
        f.write('hash\tkind\tstatus\tmessage\tspirv_modules\tspirv_val\tlogged\tname\n')
        for i in keep:
            j, r = jobs[i], results[i]
            f.write('\t'.join([j['hash'], j['kind'], r['status'], r['message'], str(r.get('spirv', 0)),
                               ' | '.join(r.get('spirv_failures', [])) or 'ok',
                               ' | '.join(sorted(set(r['errors']))), j['name']]) + '\n')
    counts = collections.Counter((jobs[i]['kind'], results[i]['status']) for i in keep)
    statuses = sorted({s for _, s in counts})
    kinds = [k for k in STAGE_NAMES if any((k, s) in counts for s in statuses)]
    loud = collections.defaultdict(list)
    for i in keep:
        if results[i]['status'] != 'PASS':
            loud[(results[i]['status'], normalize(results[i]['message']))].append(i)
    val = collections.defaultdict(list)
    for i in keep:
        for failure in results[i].get('spirv_failures', []):
            val[normalize(failure.split(': ', 1)[1])].append(i)
    logged = collections.defaultdict(list)
    for i in keep:
        if results[i]['status'] == 'PASS':
            for e in set(results[i]['errors']):
                logged[normalize(e)].append(i)
    lines = ['# Shader corpus sweep (Apple)', '',
             f'Profile: {results.get("profile", "?")}. {len(keep)} jobs in {compile_seconds:.0f} s '
             f'(compile) / {total_seconds:.0f} s (total).', '',
             'Registers come from each shader\'s Gnmx stage struct; user data is synthesized from '
             'its input usage slots (valid V#/T#/S#, tables of 2D RGBA8 T#s or float4 V#s, a '
             'generated fetch shader). Draw-time state the files do not carry is representative: '
             'PS render targets follow the export format, VS clip space 0..W, GS input topology '
             'from its ESGS ring loads, ESGS item size from the ES shipped under the GS\'s name, '
             'tessellation constants for one patch. A PASS is a compile under these registers, '
             'not under every permutation the game draws.', '',
             '| stage | ' + ' | '.join(statuses) + ' |', '|---|' + '---|' * len(statuses)]
    for k in kinds:
        lines.append(f'| {STAGE_NAMES[k]} | ' + ' | '.join(str(counts.get((k, s), 0)) for s in statuses) + ' |')
    lines += ['', '## Not PASS, grouped by message (numbers folded to N)', '']
    for (status, key), indices in sorted(loud.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'### {status} x{len(indices)}: {key}')
        exact = collections.Counter(results[i]['message'] for i in indices)
        for message, n in exact.most_common(5):
            lines.append(f'- exact ({n}): `{message}`')
        lines.append('- shaders: ' + ', '.join(
            f'{jobs[i]["hash"]} {jobs[i]["kind"]} ({jobs[i]["name"].split(":")[-1]})' for i in indices[:12])
            + (f', ... {len(indices) - 12} more' if len(indices) > 12 else ''))
        lines.append('')
    lines += ['## spirv-val failures (first error, numbers folded)', '']
    for key, indices in sorted(val.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'- x{len(indices)}: `{key}` e.g. ' + ', '.join(
            f'{jobs[i]["hash"]} {jobs[i]["kind"]}' for i in indices[:6]))
    lines += ['', '## Errors and warnings logged by jobs that compiled', '']
    for key, indices in sorted(logged.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'- x{len(indices)}: `{key}` e.g. ' + ', '.join(
            f'{jobs[i]["hash"]} {jobs[i]["kind"]}' for i in indices[:6]))
    with open(os.path.join(out, 'report.md'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    write_census(out, jobs, keep, results)
    print('\n'.join(lines[:12 + len(kinds)]))
    print(f'-> {out}/report.md, results.tsv, gs_census.md')


def write_census(out, jobs, keep, results):
    prims = {1: 'points', 2: 'lines', 4: 'triangles'}
    out_prims = {0: 'points', 1: 'line strip', 2: 'triangle strip'}
    rows = []
    for i in keep:
        j = jobs[i]
        if j['kind'] != 'gs':
            continue
        kv = {k: v[0] for k, v in j['kv'].items() if k not in ('slot', 'copy_slot', 'sem')}
        g = lambda k: int(kv[k], 16)
        r = results[i]
        streams = [g(f'vert_itemsize{s}') for s in range(4)]
        rows.append({
            'hash': j['hash'], 'name': j['name'].split(':')[-1].split('\\')[-1], 'files': j['files'],
            'in': prims.get(r['gs'].get('in_prim'), '?'),
            'out': out_prims.get(g('out_prim_type') & 0x3F, hex(g('out_prim_type'))),
            'max_vert_out': g('max_vert_out'), 'instances': g('instance_cnt'),
            'strmout': g('strmout_config'), 'streams': '/'.join(str(s) for s in streams),
            'esgs': g('esgs'), 'es': kv['es'], 'copy_params': (g('copy_vs_out_config') >> 1 & 0x1F) + 1,
            'verdict': r['status'] if r['status'] != 'PASS' else 'PASS (compiled)',
            'message': r['message']})
    configs = collections.Counter((r['in'], r['out'], r['max_vert_out'], r['streams'], r['instances'],
                                   r['strmout'], r['verdict'] + (': ' + r['message'] if r['message'] else ''))
                                  for r in rows)
    lines = ['# Static GS census: every ES/GS/copy configuration in the game data', '',
             f'{len(rows)} GS jobs ({len({r["hash"] for r in rows})} GS binaries; one job per ESGS item '
             'size of the ES shipped with it). Input topology is inferred from the vertex offset '
             'VGPRs of the GS\'s ESGS ring loads; "es=unpaired" means no ES ships under the GS\'s '
             'name and the ESGS size is its output vertex size.', '',
             '## Configurations', '',
             '| in | out | max verts | vert dwords per stream | GS instances reg | stream-out reg | verdict | GS jobs |',
             '|---|---|---|---|---|---|---|---|']
    for (inp, outp, mvo, streams, inst, so, verdict), n in sorted(configs.items(), key=lambda kv: -kv[1]):
        lines.append(f'| {inp} | {outp} | {mvo} | {streams} | {inst:#x} | {so:#x} | {verdict} | {n} |')
    lines += ['', '## Shaders', '',
              '| GS hash | name | files | in | out | max verts | streams | esgs | ES | copy params | verdict |',
              '|---|---|---|---|---|---|---|---|---|---|---|']
    for r in sorted(rows, key=lambda r: (r['verdict'], r['name'])):
        lines.append(f'| {r["hash"]} | {r["name"]} | {r["files"]} | {r["in"]} | {r["out"]} | '
                     f'{r["max_vert_out"]} | {r["streams"]} | {r["esgs"]} | {r["es"].replace(",", " ")} | '
                     f'{r["copy_params"]} | {r["verdict"]}{": " + r["message"] if r["message"] else ""} |')
    with open(os.path.join(out, 'gs_census.md'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
