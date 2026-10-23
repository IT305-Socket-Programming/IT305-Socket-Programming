#!/usr/bin/env python3
"""IT305 preliminary evaluator, based on the assignment and sample design.

Python 3.9+, Linux, standard library only. This is NOT the professor's final key.
No code runs at import. Run --help for CLI options. Required program invocations:
  Part1/server PORT TOPICS_ROOT
  Part2_*/server PORT TOPICS_ROOT PROBABILITY
  */client IP PORT TOPIC OUTPUT_ROOT

Examples (in a Linux terminal at the submission root):
  python3 test_universal.py . --quick
  python3 test_universal.py . /path/to/raw-img
  python3 test_universal.py . /path/to/dataset --only Part2_Case2
  python3 test_universal.py . --submission --group 005 102
  python3 test_universal.py . --config /path/to/options.json --adapter /path/to/adapter.py

The optional dataset positional argument accepts the topic root or a parent with
raw-img/. Synthetic topics are ALWAYS generated, including binary and boundary
sized files, so an absent official dataset does not prevent preliminary testing.
--quick samples at most two external topics and reduces load; the report says so.

CORE: required folders/files, make exit status and named runnable programs,
mandatory CLI, fresh runtime roots/ports/topics, all-file hashes, independent
concurrent outputs, and recovery after actual proxy disconnections in Part II.
DIAGNOSTIC: random fault probabilities and observable timings. A timeout under
random faults is not proof of an invalid recovery implementation.
OPTIONAL: empty topics, unusual names, overwrite policy, malformed CLI, repeated
connections, transport fragmentation. These never reject a submission by default.

Wire encoding, mode options, checkpoint interval, error numbers, CSV schema,
session takeover, symlink policy, SIGTERM behavior, retry count, and sanitizer
flags are NOT standardized. No private protocol or invented trailing flags are
required. Protocol/session/threading/measurement/report/LAN checks are explicitly
UNVERIFIED (SKIP) until an implementation-aware adapter supplies evidence.
Client overlap and redundant wire bytes do not prove server threading or resume.

Optional JSON config (all arguments are literal argv entries, NEVER shell code):
  {"parts": {"Part1": {"server_args": [], "client_args": [],
     "modes": {"single": ["DOCUMENTED_OPTION"], "multi": ["DOCUMENTED_OPTION"]}}}}
Only use options actually documented by the submission. Baseline tests ALWAYS
use mandatory CLI alone. Configured modes are additional functional checks;
selecting a mode does not prove its internal execution model.

Adapter contract (trusted local Python; invoked once per successfully built part):
  def run_checks(context):
      yield {"capability": "protocol", "name": "fragmented GET",
             "status": "PASS", "evidence": "...observed assertions..."}
Recognized capabilities: protocol, threading, part1_modes, case1_restart,
case2_resume, enhancement, metrics, report_review, lan_experiments.
Statuses: PASS, FAIL, WARN, SKIP. Every result needs nonempty evidence. FAIL affects
exit status. A capability is verified only if ALL of its results PASS. Keep an
adapter's scenarios specific: cover malformed headers/lengths, wrong transactions,
invalid ACKs/checkpoints, interrupted handshakes/metadata/data/ACK/completion,
and every file in races; include a valid positive control. Do not assert one
takeover policy, checkpoint granularity, or p=1 outcome unless documented.
Context attributes: part, root, case_dir, work, logs, topics_root, topic_names,
bulk_topic, options, config, run; methods server(root=None, probability=0,
extra=()), client(port, topic, output, extra=()), verify(topic, output, root=None),
record(name,status,evidence,scope='adapted'). server() is a context manager;
client() returns a dict with rc, timeout, seconds, stdout, stderr, argv (paths to
file-backed logs, not captured PIPEs). All adapter children MUST use run.spawn
or context managers; run.wait(process, timeout) enforces group cleanup. Adapters
must not mutate the input dataset. Return an iterable, not a precomputed verdict.

Results: unique directory under --results-dir (default ROOT/it305-test-results),
report.json, results.csv, measurements.csv, summary.txt, and per-process logs.
Received outputs are removed after verification unless --keep-output; logs stay.
Generated datasets/work are retained only with --keep-output. Builds run in each
case directory and may update binaries/object files. No make clean is assumed.
Input datasets are never modified. Do not use reports as an official grade.

Exit 0: executed core/adapted checks pass (possibly PARTIAL coverage).
Exit 1: core/adapted failure. Exit 2: evaluator/setup error.
Exit 3: --require-complete and required evidence is unavailable. Exit 130: Ctrl-C.
Linux loopback verifies functionality; it cannot replace the assigned experiment
with one server computer and two client computers, nor establish LAN speedups.
"""

import argparse
import concurrent.futures
import contextlib
import csv
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
import zipfile
import tarfile

PARTS = ('Part1', 'Part2_Case1', 'Part2_Case2', 'Part2_Case2_Enhanced')
CAPABILITIES = {'protocol', 'threading', 'part1_modes', 'case1_restart',
                'case2_resume', 'enhancement', 'metrics', 'report_review',
                'lan_experiments'}
STATUSES = {'PASS', 'FAIL', 'WARN', 'SKIP', 'ERROR'}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def manifest(topic):
    """Direct regular files only; symlink policy is deliberately not inferred."""
    if not topic.is_dir() or topic.is_symlink():
        raise ValueError('topic is not a real directory: ' + str(topic))
    files = {}
    for p in sorted(topic.iterdir()):
        mode = p.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise ValueError('dataset contains a symlink; choose a link-free topic root: ' + str(p))
        if stat.S_ISREG(mode):
            files[p.name] = {'size': p.stat().st_size, 'sha256': digest(p)}
        elif not stat.S_ISDIR(mode):
            raise ValueError('dataset has a non-regular entry: ' + str(p))
    return files


def compare(expected, output, topic):
    """Return evidence, checking types before opening, never following output links."""
    dst = output / topic
    if output.is_symlink() or dst.is_symlink() or not dst.is_dir():
        return False, 'missing, linked, or non-directory output/topic'
    got = {p.name: p for p in dst.iterdir()}
    missing, extra = sorted(set(expected) - set(got)), sorted(set(got) - set(expected))
    if missing or extra:
        return False, 'file names differ: missing=%r extra=%r' % (missing[:8], extra[:8])
    for name, info in expected.items():
        p = got[name]
        if not stat.S_ISREG(p.lstat().st_mode):
            return False, 'output is not a regular file: ' + name
        if p.stat().st_size != info['size'] or digest(p) != info['sha256']:
            return False, 'size or SHA-256 differs: ' + name
    siblings = sorted(p.name for p in output.iterdir() if p.name != topic)
    if siblings:
        return False, 'unexpected entries beside topic: %r' % siblings[:8]
    return True, '%d files, %d useful bytes, every SHA-256 matches' % (
        len(expected), sum(v['size'] for v in expected.values()))


def remove_owned(path, base):
    """Only remove generated work beneath this run's unique directory."""
    absolute = path.absolute()
    # Check the parent before resolving the leaf: an output symlink should be
    # unlinked, never followed to (or recursively removed at) its target.
    if absolute == base.absolute() or base.resolve() not in (absolute.parent.resolve(), *absolute.parent.resolve().parents):
        raise ValueError('refusing cleanup outside generated work: ' + str(path))
    if path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


class Runner:
    def __init__(self, logs):
        self.logs = logs
        self.lock = threading.Lock()
        self.counter = 0
        self.active = set()

    def spawn(self, argv, cwd, tag):
        with self.lock:
            self.counter += 1
            prefix = self.logs / ('%04d-%s' % (self.counter, re.sub(r'[^\w.-]', '_', tag)))
        out, err = prefix.with_suffix('.stdout.log'), prefix.with_suffix('.stderr.log')
        with out.open('wb') as so, err.open('wb') as se:
            p = subprocess.Popen([str(x) for x in argv], cwd=str(cwd), stdin=subprocess.DEVNULL,
                                 stdout=so, stderr=se, start_new_session=True)
        p.it305 = {'argv': [str(x) for x in argv], 'stdout': str(out), 'stderr': str(err),
                   'started': time.monotonic()}
        with self.lock:
            self.active.add(p)
        return p

    def stop(self, p):
        # Signal the group even if the parent has exited: wrappers can leave children.
        with contextlib.suppress(ProcessLookupError):
            os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        with contextlib.suppress(ProcessLookupError):
            os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        with self.lock:
            self.active.discard(p)

    def wait(self, p, timeout):
        expired = False
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            expired = True
        finally:
            self.stop(p)
        return dict(p.it305, rc=p.returncode, timeout=expired,
                    seconds=time.monotonic() - p.it305['started'])

    def call(self, argv, cwd, tag, timeout):
        return self.wait(self.spawn(argv, cwd, tag), timeout)

    def close(self):
        with self.lock:
            processes = list(self.active)
        for p in processes:
            self.stop(p)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, ctx, root, probability, extra):
        self.ctx, self.root, self.probability, self.extra = ctx, root, probability, extra
        self.proc = None

    def __enter__(self):
        # No race-free port reservation is possible with the specified CLI.
        # Retry startup on different ports; preserve every attempt's logs.
        for _ in range(3):
            self.port = free_port()
            args = [self.ctx.case_dir / 'server', self.port, self.root]
            if self.ctx.part != 'Part1':
                args.append(str(self.probability))
            args.extend(self.extra)
            self.proc = self.ctx.run.spawn(args, self.ctx.case_dir, self.ctx.part + '-server')
            deadline = time.monotonic() + self.ctx.options.startup_timeout
            while time.monotonic() < deadline and self.proc.poll() is None:
                try:
                    with socket.create_connection(('127.0.0.1', self.port), timeout=0.15):
                        pass
                    # Readiness connections contain no request and must not count
                    # as transfer attempts or injected-failure evidence.
                    time.sleep(0.05)
                    if self.proc.poll() is None:
                        return self
                except OSError:
                    pass
                time.sleep(0.03)
            self.ctx.run.stop(self.proc)
        raise RuntimeError('server did not remain ready; see server logs')

    def __exit__(self, *exc):
        self.ctx.run.stop(self.proc)


class Proxy:
    """Opaque TCP relay: actual bounded cuts, optional fragmented writes.

    Counts application TCP-stream bytes, including metadata, not packet headers
    or TCP retransmissions. It never interprets the submission's wire format.
    Cut location therefore cannot be labeled a DATA/ACK/checkpoint boundary.
    """
    def __init__(self, upstream, cuts=0, after=131072, fragment=False):
        self.upstream, self.remaining = upstream, cuts
        self.after, self.fragment = after, fragment
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.sockets, self.threads = set(), []
        self.errors = []
        self.stats = {'connections': 0, 'cuts': 0, 'server_stream_bytes': 0,
                      'client_stream_bytes': 0}

    def __enter__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(64)
        self.listener.settimeout(0.2)
        self.acceptor = threading.Thread(target=self.accept, daemon=True)
        self.acceptor.start()
        return self

    def accept(self):
        while not self.done.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                upstream = socket.create_connection(('127.0.0.1', self.upstream), timeout=2)
                upstream.settimeout(0.2)
                client.settimeout(0.2)
            except OSError as e:
                client.close()
                with self.lock:
                    self.errors.append(str(e))
                continue
            with self.lock:
                self.stats['connections'] += 1
                self.sockets.update((client, upstream))
            closed = threading.Event()
            for src, dst, downstream in ((upstream, client, True), (client, upstream, False)):
                th = threading.Thread(target=self.relay, args=(src, dst, downstream, closed), daemon=True)
                with self.lock:
                    self.threads.append(th)
                th.start()

    def relay(self, src, dst, downstream, closed):
        seen = 0
        key = 'server_stream_bytes' if downstream else 'client_stream_bytes'
        try:
            while not self.done.is_set() and not closed.is_set():
                try:
                    data = src.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    with contextlib.suppress(OSError):
                        dst.shutdown(socket.SHUT_WR)
                    return
                cut = False
                if downstream and seen + len(data) >= self.after:
                    with self.lock:
                        if self.remaining > 0:
                            self.remaining -= 1
                            cut = True
                    if cut:
                        data = data[:max(0, self.after - seen)]
                step = 97 if self.fragment and seen < 8192 else 65536
                for offset in range(0, len(data), step):
                    piece = memoryview(data)[offset:offset + step]
                    while piece and not self.done.is_set() and not closed.is_set():
                        try:
                            sent = dst.send(piece)
                        except socket.timeout:
                            continue
                        if sent == 0:
                            raise ConnectionError('relay send returned zero')
                        piece = piece[sent:]
                        seen += sent
                        with self.lock:
                            self.stats[key] += sent
                    if self.fragment and seen < 8192:
                        time.sleep(0.0005)
                if cut:
                    with self.lock:
                        self.stats['cuts'] += 1
                    closed.set()
                    for s in (src, dst):
                        with contextlib.suppress(OSError):
                            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                            s.close()
                    return
        except OSError:
            closed.set()
            for s in (src, dst):
                with contextlib.suppress(OSError):
                    s.shutdown(socket.SHUT_RDWR)

    def __exit__(self, *exc):
        self.done.set()
        self.listener.close()
        self.acceptor.join(timeout=3)
        with self.lock:
            sockets, threads = list(self.sockets), list(self.threads)
        for s in sockets:
            with contextlib.suppress(OSError):
                s.shutdown(socket.SHUT_RDWR)
            s.close()
        for th in threads:
            th.join(timeout=1)
        if self.acceptor.is_alive() or any(th.is_alive() for th in threads):
            self.errors.append('relay thread did not stop')


class Context:
    def __init__(self, suite, part):
        self.suite, self.part = suite, part
        self.root, self.options, self.run = suite.root, suite.options, suite.run
        self.case_dir = self.root / part
        self.logs, self.work = suite.logs, suite.work
        self.topics_root = suite.synthetic
        self.topic_names, self.bulk_topic = suite.topics, suite.bulk
        self.config = suite.config.get('parts', {}).get(part, {})

    def server(self, root=None, probability=0, extra=()):
        return Server(self, root or self.topics_root, probability, extra)

    def client(self, port, topic, output, extra=()):
        return self.run.call([self.case_dir / 'client', '127.0.0.1', port, topic, output, *extra],
                             self.case_dir, self.part + '-client', self.options.timeout)

    def verify(self, topic, output, root=None):
        return compare(self.suite.expected(root or self.topics_root, topic), Path(output), topic)

    def record(self, name, status, evidence, scope='adapted'):
        self.suite.record(self.part, name, status, evidence, scope)


class Suite:
    def __init__(self, options):
        self.options = options
        self.root = options.root.resolve()
        parent = options.results_dir.resolve() if options.results_dir else self.root / 'it305-test-results'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'), dir=parent))
        self.logs = self.directory / 'logs'
        self.work = self.directory / 'work'
        self.logs.mkdir()
        self.work.mkdir()
        self.run = Runner(self.logs)
        self.results, self.measurements, self.cache = [], [], {}
        self.config = json.loads(options.config.read_text()) if options.config else {}
        self.validate_config()
        self.adapter = None
        if options.adapter:
            spec = importlib.util.spec_from_file_location('it305_local_adapter', options.adapter.resolve())
            self.adapter = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.adapter)
            if not callable(getattr(self.adapter, 'run_checks', None)):
                raise ValueError('adapter must define run_checks(context)')
        self.synthetic = self.work / ('dataset-' + uuid.uuid4().hex[:12])
        self.synthetic.mkdir()
        prefix = 'probe' + uuid.uuid4().hex[:10]
        self.topics = [prefix + 'binary', prefix + 'many', prefix + 'bulk']
        self.bulk = self.topics[-1]
        rng = random.Random(305)
        for topic, sizes in zip(self.topics, ([0, 1, 4095, 4096, 4097, 65535, 65536, 65537, 1048576],
                                             [37 + i * 113 for i in range(24)],
                                             [2 * 1024 * 1024 if options.quick else 8 * 1024 * 1024])):
            folder = self.synthetic / topic
            folder.mkdir()
            for i, size in enumerate(sizes):
                with (folder / ('file%03d.bin' % i)).open('wb') as f:
                    while size:
                        n = min(size, 65536)
                        f.write(bytes(rng.getrandbits(8) for _ in range(n)))
                        size -= n
        self.edge_topics = [prefix + 'empty', prefix + 'names']
        for topic in self.edge_topics:
            (self.synthetic / topic).mkdir()
        for name in ('a space.txt', 'utf8-\u03b2.bin', '.hidden', 'line\nbreak.bin'):
            (self.synthetic / self.edge_topics[1] / name).write_bytes(b'\x00\xff\r\n' * 17)

    def validate_config(self):
        if not isinstance(self.config, dict) or set(self.config) - {'parts'}:
            raise ValueError('config accepts only a parts object')
        parts = self.config.get('parts', {})
        if not isinstance(parts, dict) or set(parts) - set(PARTS):
            raise ValueError('config has unknown parts')
        for part, value in parts.items():
            if not isinstance(value, dict) or set(value) - {'server_args', 'client_args', 'modes'}:
                raise ValueError('invalid config for ' + part)
            for key in ('server_args', 'client_args'):
                if not isinstance(value.get(key, []), list) or not all(isinstance(x, str) for x in value.get(key, [])):
                    raise ValueError(key + ' must be an array of argv strings')
            modes = value.get('modes', {})
            if not isinstance(modes, dict) or any(not isinstance(k, str) or not isinstance(v, list)
                or not all(isinstance(a, str) for a in v) for k, v in modes.items()):
                raise ValueError('modes must map names to argv arrays')

    def expected(self, root, topic):
        key = (str(root), topic)
        if key not in self.cache:
            self.cache[key] = manifest(root / topic)
        return self.cache[key]

    def output(self):
        return self.work / ('output-' + uuid.uuid4().hex)

    def record(self, part, name, status, evidence, scope='core', **extra):
        if status not in STATUSES or scope not in {'core', 'adapted', 'optional', 'diagnostic'}:
            raise ValueError('invalid result status or scope')
        row = dict(part=part, name=name, status=status, scope=scope, evidence=str(evidence), **extra)
        self.results.append(row)
        print('%-5s [%s] %s / %s: %s' % (status, scope, part, name, str(evidence)[:600]), flush=True)

    def isolated(self, part, name, fn, scope='core'):
        try:
            return fn()
        except Exception as e:
            trace = self.logs / ('exception-' + uuid.uuid4().hex + '.log')
            trace.write_text(traceback.format_exc(), encoding='utf-8')
            status = 'FAIL' if isinstance(e, (OSError, RuntimeError, subprocess.SubprocessError)) else 'ERROR'
            self.record(part, name, status, '%s: %s; traceback=%s' % (type(e).__name__, e, trace), scope)
            return None

    def layout(self):
        for part in PARTS:
            self.record('submission', part, 'PASS' if (self.root / part).is_dir() else 'FAIL',
                        'required case directory')
        for name in ('README.md', 'Results'):
            p = self.root / name
            ok = p.is_file() and p.stat().st_size > 0 if name.endswith('.md') else p.is_dir()
            self.record('submission', name, 'PASS' if ok else 'FAIL', 'presence/type only; content requires review')
        reports = list(self.root.glob('FinalReport_Group_[0-9][0-9][0-9]_[0-9][0-9][0-9].pdf'))
        if self.options.group:
            reports = [self.root / ('FinalReport_Group_%s_%s.pdf' % tuple(self.options.group))]
        valid = []
        for p in reports:
            if p.is_file() and p.stat().st_size > 5:
                with p.open('rb') as f:
                    if f.read(5) == b'%PDF-':
                        valid.append(p)
        self.record('submission', 'final report', 'PASS' if len(valid) == 1 else 'FAIL',
                    'exactly one matching nonempty PDF header required; found %d; sections not evaluated' % len(valid))
        results = self.root / 'Results'
        for kind, suffixes in (('raw CSV logs', {'.csv'}), ('graphs', {'.png', '.pdf'})):
            found = [p for p in results.rglob('*') if p.is_file() and p.suffix.lower() in suffixes and p.stat().st_size] if results.is_dir() else []
            self.record('submission', kind, 'PASS' if found else 'FAIL',
                        '%d nonempty artifacts; per-case coverage/measurement correctness unverified' % len(found))
        if self.options.submission:
            expected = 'FinalSubmission_Group_%s_%s' % tuple(self.options.group) if self.options.group else None
            ok = self.root.name == expected if expected else bool(re.fullmatch(r'FinalSubmission_Group_\d{3}_\d{3}', self.root.name))
            self.record('submission', 'extracted root name', 'PASS' if ok else 'FAIL', str(self.root.name))
        else:
            self.record('submission', 'archive/extracted root naming', 'SKIP',
                        'repository mode; use --submission --group XXX YYY and --archive for final packaging', 'diagnostic')
        if self.options.archive:
            self.isolated('submission', 'archive', self.archive)

    def archive(self):
        path = self.options.archive.resolve()
        match = re.fullmatch(r'(FinalSubmission_Group_(\d{3})_(\d{3}))\.(zip|tar\.gz)', path.name)
        if not match:
            self.record('submission', 'archive naming', 'FAIL', path.name)
            return
        expected = match.group(1)
        if self.options.group and (match.group(2), match.group(3)) != tuple(self.options.group):
            self.record('submission', 'archive group numbers', 'FAIL', 'archive does not match --group')
        if path.suffix == '.zip':
            with zipfile.ZipFile(path) as z:
                members = z.namelist()
                bad_crc = z.testzip()
                links = [i.filename for i in z.infolist() if stat.S_ISLNK(i.external_attr >> 16)]
        else:
            with tarfile.open(path, 'r:gz') as t:
                entries = t.getmembers()
                members = [i.name for i in entries]
                links = [i.name for i in entries if not (i.isfile() or i.isdir())]
                # tar has no per-file checksum: opening validates headers, not all contents.
                bad_crc = None
        normalized = [n.removeprefix('./').rstrip('/') for n in members]
        safe = bool(normalized) and all(n and '\\' not in n and not n.startswith('/')
            and not re.match(r'^[A-Za-z]:', n) and '..' not in n.split('/')
            and n.split('/')[0] == expected for n in normalized)
        names = set(normalized)
        required = [expected + '/README.md', expected + '/FinalReport_Group_%s_%s.pdf' % (match.group(2), match.group(3))]
        missing = [n for n in required if n not in names]
        for folder in (*PARTS, 'Results'):
            if not any(n.startswith(expected + '/' + folder + '/') for n in normalized):
                missing.append(folder + '/')
        ok = safe and not missing and not bad_crc
        self.record('submission', 'archive members', 'PASS' if ok else 'FAIL',
                    'safe root=%s missing=%r CRC=%r special/linked=%r; archive not extracted or executed; bytes not compared to working tree' % (safe, missing, bad_crc, links[:5]))
        if links:
            self.record('submission', 'archive special/linked entries', 'WARN',
                        'targets/portability require review; no universal symlink policy assumed: %r' % links[:8], 'diagnostic')

    def build(self, ctx):
        if not ctx.case_dir.is_dir():
            ctx.record('build', 'SKIP', 'case directory unavailable', 'core')
            return False
        def executables():
            return {p.name: (p.stat().st_mtime_ns, p.stat().st_size)
                    for p in ctx.case_dir.iterdir() if p.is_file() and os.access(p, os.X_OK)}

        before = executables()
        if not self.options.no_build:
            for iteration in (1, 2):
                r = self.run.call(['make'], ctx.case_dir, ctx.part + '-make%d' % iteration, self.options.build_timeout)
                ok = r['rc'] == 0 and not r['timeout']
                self.record(ctx.part, 'make #%d' % iteration, 'PASS' if ok else 'FAIL',
                            'rc=%s timeout=%s; output wording/warnings are not grading criteria' % (r['rc'], r['timeout']), process=r)
                if not ok:
                    return False
            after = executables()
            unexpected = sorted(n for n, info in after.items()
                                if n not in {'server', 'client'} and before.get(n) != info)
            ctx.record('exactly two executable build outputs', 'FAIL' if unexpected else 'PASS',
                       'additional new/updated executables=%r; pre-existing unchanged helper scripts ignored' % unexpected, 'core')
            if unexpected:
                return False
        else:
            ctx.record('make', 'SKIP', '--no-build: compilation not evaluated', 'core')
        ok = all((ctx.case_dir / n).is_file() and os.access(ctx.case_dir / n, os.X_OK) for n in ('server', 'client'))
        ctx.record('runnable server/client', 'PASS' if ok else 'FAIL', 'required exact executable names', 'core')
        # Existing executable helper/source scripts are not presumed build outputs.
        return ok

    def transfer(self, ctx, port, topic, root, label, scope='core', extra=()):
        expected = self.expected(root, topic)  # Validate fixture BEFORE launching a client.
        output = self.output()
        try:
            r = ctx.client(port, topic, output, extra)
            ok, detail = compare(expected, output, topic)
            status = 'FAIL' if r['timeout'] or not ok else ('PASS' if r['rc'] == 0 else 'WARN')
            self.record(ctx.part, label, status, 'rc=%s timeout=%s; %s' % (r['rc'], r['timeout'], detail), scope, process=r)
            useful = sum(v['size'] for v in expected.values())
            self.measurements.append(dict(part=ctx.part, test=label, scope=scope, status=status,
                seconds=r['seconds'], useful_bytes=useful if ok else 0,
                useful_MiB_s=useful / r['seconds'] / 1048576 if ok and not r['timeout'] else None,
                note='wall time includes client startup; verified completed bytes only; loopback'))
            return ok and not r['timeout'], r
        finally:
            if not self.options.keep_output and output.exists():
                remove_owned(output, self.work)

    def functional(self, ctx):
        with ctx.server() as server:
            for topic in self.topics:
                self.isolated(ctx.part, 'synthetic ' + topic,
                    lambda t=topic: self.transfer(ctx, server.port, t, self.synthetic, 'synthetic ' + t))
            missing = 'absent' + uuid.uuid4().hex
            output = self.output()
            try:
                r = ctx.client(server.port, missing, output)
                files = list(output.rglob('*')) if output.exists() and output.is_dir() else []
                unexpected = [str(p) for p in files if p.is_file() or p.is_symlink()]
                status = 'FAIL' if unexpected else ('WARN' if r['timeout'] or r['rc'] == 0 else 'PASS')
                self.record(ctx.part, 'unknown topic', status,
                    'rc=%s timeout=%s unexpected files=%r; no exact error number/text required' % (r['rc'], r['timeout'], unexpected[:5]), process=r)
            finally:
                if not self.options.keep_output and output.exists():
                    remove_owned(output, self.work)
            self.transfer(ctx, server.port, self.topics[0], self.synthetic, 'health after unknown topic')
        if self.options.dataset:
            root = self.options.dataset.resolve()
            if (root / 'raw-img').is_dir():
                root = root / 'raw-img'
            if not root.is_dir():
                raise ValueError('external dataset root is not a directory: ' + str(root))
            topics = sorted(p.name for p in root.iterdir() if p.is_dir() and not p.is_symlink())
            if not topics:
                raise ValueError('external dataset has no topic directories (cannot pass vacuously)')
            selected = topics[:2] if self.options.quick else topics
            self.record(ctx.part, 'external dataset coverage', 'WARN' if len(selected) < len(topics) else 'PASS',
                        '%d/%d topics selected' % (len(selected), len(topics)), 'diagnostic')
            with ctx.server(root=root) as server:
                for topic in selected:
                    self.isolated(ctx.part, 'external ' + topic,
                        lambda t=topic: self.transfer(ctx, server.port, t, root, 'external ' + t))
        else:
            ctx.record('official dataset', 'SKIP', 'not provided; synthetic coverage only', 'diagnostic')

    def concurrent(self, ctx):
        count = self.options.clients
        with ctx.server() as server:
            started = time.monotonic()
            jobs = []
            try:
                for i in range(count):
                    topic, output = self.topics[i % len(self.topics)], self.output()
                    p = self.run.spawn([ctx.case_dir / 'client', '127.0.0.1', server.port, topic, output],
                                       ctx.case_dir, ctx.part + '-concurrent')
                    jobs.append((topic, output, p))
                deadline = started + self.options.timeout
                bad, nonzero = [], []
                for topic, output, p in jobs:
                    r = self.run.wait(p, max(0.01, deadline - time.monotonic()))
                    ok, reason = ctx.verify(topic, output)
                    if not ok or r['timeout']:
                        bad.append(dict(topic=topic, reason=reason, process=r))
                    elif r['rc'] != 0:
                        nonzero.append(r['rc'])
                status = 'FAIL' if bad else ('WARN' if nonzero else 'PASS')
                self.record(ctx.part, 'independent simultaneous client launches', status,
                            '%d clients; %d incorrect/timed out; nonzero=%r; completion does not prove parallel service' % (count, len(bad), nonzero), failures=bad)
                elapsed = time.monotonic() - started
                useful = sum(sum(v['size'] for v in self.expected(self.synthetic, t).values()) for t, _, _ in jobs)
                self.measurements.append(dict(part=ctx.part, test='concurrent client batch', scope='core',
                    status=status, seconds=elapsed, useful_bytes=useful if not bad else 0,
                    useful_MiB_s=useful / elapsed / 1048576 if not bad else None,
                    note='aggregate completed useful bytes / whole batch wall time; not evidence of threading'))
            finally:
                for _, output, p in jobs:
                    self.run.stop(p)
                    if not self.options.keep_output and output.exists():
                        remove_owned(output, self.work)

    def proxy_check(self, ctx, fragmented=False):
        scope = 'optional' if fragmented else 'core'
        label = 'fragmented opaque TCP relay' if fragmented else 'recovery after forced TCP cuts'
        with ctx.server() as server:
            with Proxy(server.port, cuts=0 if fragmented else 2, fragment=fragmented) as proxy:
                ok, r = self.transfer(ctx, proxy.port, self.bulk, self.synthetic, label, scope)
            stats = dict(proxy.stats)
            evidence = '%s; proxy errors=%r; counters include protocol metadata' % (stats, proxy.errors)
            if proxy.errors:
                self.record(ctx.part, label + ' evidence', 'ERROR', evidence, scope)
            elif not fragmented and stats['cuts'] == 0:
                self.record(ctx.part, label + ' evidence', 'WARN', 'no cut reached: recovery UNVERIFIED; ' + evidence, scope)
            else:
                self.record(ctx.part, label + ' evidence', 'PASS' if ok else 'FAIL', evidence, scope)
            self.measurements.append(dict(part=ctx.part, test=label, scope=scope,
                status='PASS' if ok and not proxy.errors and (fragmented or stats['cuts']) else 'WARN',
                seconds=r['seconds'], useful_bytes=sum(v['size'] for v in self.expected(self.synthetic, self.bulk).values()) if ok else 0,
                server_stream_bytes=stats['server_stream_bytes'], client_stream_bytes=stats['client_stream_bytes'],
                cuts=stats['cuts'], connections=stats['connections'],
                note='opaque relay totals, not DATA/redundant-file-byte metrics; no restart/resume proof'))

    def random_faults(self, ctx):
        for probability in self.options.fault_prob:
            trials = 1 if self.options.quick else self.options.fault_trials
            for trial in range(trials):
                label = 'native probability %g trial %d' % (probability, trial + 1)
                # Random fault outcomes cannot universally reject a submission:
                # injection cadence and retry defaults are implementation choices.
                with ctx.server(probability=probability) as server:
                    output = self.output()
                    try:
                        r = ctx.client(server.port, self.topics[0], output)
                        ok, reason = ctx.verify(self.topics[0], output)
                        self.record(ctx.part, label, 'PASS' if ok and not r['timeout'] and r['rc'] == 0 else 'WARN',
                            'rc=%s timeout=%s; %s; actual native injection UNVERIFIED without adapter' % (r['rc'], r['timeout'], reason), 'diagnostic', process=r)
                    finally:
                        if not self.options.keep_output and output.exists():
                            remove_owned(output, self.work)

    def optional(self, ctx):
        with ctx.server() as server:
            for topic in self.edge_topics:
                self.isolated(ctx.part, 'edge ' + topic,
                    lambda t=topic: self.transfer(ctx, server.port, t, self.synthetic, 'edge ' + t, 'optional'), 'optional')
            output = self.output()
            topic = self.topics[0]
            (output / topic).mkdir(parents=True)
            name = next(iter(self.expected(self.synthetic, topic)))
            (output / topic / name).write_bytes(b'old content')
            try:
                r = ctx.client(server.port, topic, output)
                ok, detail = ctx.verify(topic, output)
                ctx.record('existing output overwrite', 'PASS' if ok and not r['timeout'] else 'WARN',
                           'rc=%s; %s; overwrite policy is not standardized' % (r['rc'], detail), 'optional')
            finally:
                if not self.options.keep_output and output.exists():
                    remove_owned(output, self.work)
            blocked = self.output()
            blocked.mkdir()
            (blocked / topic).write_bytes(b'blocking file')
            try:
                r = ctx.client(server.port, topic, blocked)
                rejected = not r['timeout'] and r['rc'] != 0
                ctx.record('output/topic is a regular file', 'PASS' if rejected else 'WARN',
                           'rc=%s timeout=%s; filesystem error handling observation' % (r['rc'], r['timeout']), 'optional')
            finally:
                if not self.options.keep_output and blocked.exists():
                    remove_owned(blocked, self.work)
            def resource_sample():
                proc = Path('/proc') / str(server.proc.pid)
                try:
                    return {'fds': len(list((proc / 'fd').iterdir())),
                            'threads': len(list((proc / 'task').iterdir()))}
                except OSError:
                    return None
            before = resource_sample()
            repetitions = 4 if self.options.quick else 12
            for i in range(repetitions):
                self.isolated(ctx.part, 'repeat connection %d' % i,
                    lambda n=i: self.transfer(ctx, server.port, self.topics[1], self.synthetic,
                                              'repeat connection %d' % n, 'optional'), 'optional')
            time.sleep(0.3)
            after = resource_sample()
            growth = before is not None and after is not None and any(after[k] > before[k] + 2 for k in before)
            ctx.record('repeated-connection resource observation', 'WARN' if growth else 'SKIP',
                       'before=%r after=%r; short samples cannot prove absence of leaks; wrappers may hide worker resources' % (before, after), 'diagnostic')
        for executable in ('server', 'client'):
            r = self.run.call([ctx.case_dir / executable], ctx.case_dir,
                              ctx.part + '-missing-cli-' + executable, min(5, self.options.timeout))
            ctx.record('missing arguments ' + executable, 'PASS' if r['rc'] != 0 and not r['timeout'] else 'WARN',
                       'rc=%s timeout=%s; no exact exit status required' % (r['rc'], r['timeout']), 'optional')
        self.isolated(ctx.part, 'fragmentation', lambda: self.proxy_check(ctx, True), 'optional')

    def configured(self, ctx):
        configs = dict(ctx.config.get('modes', {}))
        if ctx.config.get('server_args') or ctx.config.get('client_args'):
            configs['configured-default'] = ctx.config.get('server_args', [])
        for name, args in configs.items():
            def check_mode(n=name, argv=args):
                with ctx.server(extra=argv) as server:
                    self.transfer(ctx, server.port, self.topics[0], self.synthetic,
                                  'configured ' + n, 'adapted', ctx.config.get('client_args', []))
            self.isolated(ctx.part, 'configured ' + name, check_mode, 'adapted')

    def adapted(self, ctx):
        needs = ['protocol', 'threading', 'metrics', 'report_review', 'lan_experiments']
        if ctx.part == 'Part1':
            needs.append('part1_modes')
        elif ctx.part == 'Part2_Case1':
            needs.append('case1_restart')
        else:
            needs.append('case2_resume')
            if ctx.part == 'Part2_Case2_Enhanced':
                needs.append('enhancement')
        observed = {}
        if self.adapter:
            try:
                entries = self.adapter.run_checks(ctx)
                for entry in entries:
                    if not isinstance(entry, dict):
                        raise ValueError('adapter result must be a dict')
                    capability = entry.get('capability')
                    if capability not in CAPABILITIES or entry.get('status') not in {'PASS', 'FAIL', 'WARN', 'SKIP'}:
                        raise ValueError('adapter result has invalid capability/status')
                    if not str(entry.get('name', '')).strip() or not str(entry.get('evidence', '')).strip():
                        raise ValueError('adapter results require a name and evidence')
                    observed.setdefault(capability, []).append(entry['status'])
                    self.record(ctx.part, 'adapter ' + entry['name'], entry['status'], entry['evidence'],
                                'adapted', capability=capability)
            except Exception as e:
                observed = {}  # A partially executed adapter cannot establish completeness.
                self.record(ctx.part, 'adapter execution', 'ERROR', repr(e), 'adapted')
        for capability in needs:
            statuses = observed.get(capability, [])
            if not statuses or any(s != 'PASS' for s in statuses):
                self.record(ctx.part, 'UNVERIFIED ' + capability, 'SKIP',
                            'needs implementation-aware observations or manual evidence; no universal PASS claimed',
                            'core', capability=capability)

    def execute(self):
        self.isolated('submission', 'layout', self.layout)
        for part in self.options.only or PARTS:
            ctx = Context(self, part)
            built = self.isolated(part, 'build', lambda: self.build(ctx))
            if not built:
                self.record(part, 'runtime coverage', 'SKIP', 'build or executable prerequisite failed')
                continue
            self.isolated(part, 'mandatory CLI and topics', lambda: self.functional(ctx))
            self.isolated(part, 'simultaneous clients', lambda: self.concurrent(ctx))
            if part != 'Part1':
                self.isolated(part, 'forced disconnect recovery', lambda: self.proxy_check(ctx))
                self.isolated(part, 'native random faults', lambda: self.random_faults(ctx), 'diagnostic')
            if self.options.robustness:
                self.isolated(part, 'optional robustness', lambda: self.optional(ctx), 'optional')
            self.configured(ctx)
            self.adapted(ctx)

    def save(self, interrupted=False):
        relevant = [r for r in self.results if r['scope'] in {'core', 'adapted'}]
        failures = [r for r in relevant if r['status'] == 'FAIL']
        errors = [r for r in relevant if r['status'] == 'ERROR']
        incomplete = [r for r in relevant if r['status'] in {'WARN', 'SKIP'}]
        exit_code = 130 if interrupted else (2 if errors else (1 if failures else (3 if incomplete and self.options.require_complete else 0)))
        verdict = 'INTERRUPTED' if interrupted else ('EVALUATOR/SETUP ERROR' if errors else ('FAIL' if failures else ('PASS - PARTIAL COVERAGE' if incomplete else 'PASS - DECLARED COVERAGE')))
        summary = ('%s\n%d results; %d core/adapted failures; %d errors; %d unavailable/inconclusive checks.\n'
                   'This is a preliminary evaluator, not the released professor key or an official grade.\n'
                   'Only selected parts ran: %s. Loopback cannot replace three-computer LAN experiments.\n'
                   'Logs and reports: %s\nExit status: %d\n') % (
                       verdict, len(self.results), len(failures), len(errors), len(incomplete),
                       ', '.join(self.options.only or PARTS), self.directory, exit_code)
        self.directory.joinpath('summary.txt').write_text(summary, encoding='utf-8')
        report = dict(format_version=1, verdict=verdict, exit_code=exit_code,
            options={k: str(v) if isinstance(v, Path) else v for k, v in vars(self.options).items()},
            results=self.results, measurements=self.measurements,
            limitations=['wire format unspecified', 'restart/resume semantics need adapter',
                        'threading/modes/enhancement need evidence', 'report contents not automatically reviewed',
                        'native injection cadence not inferred', 'three-computer experiments not performed'])
        self.directory.joinpath('report.json').write_text(json.dumps(report, indent=2, ensure_ascii=True), encoding='utf-8')
        with (self.directory / 'results.csv').open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=['part', 'name', 'scope', 'status', 'evidence'], extrasaction='ignore')
            writer.writeheader()
            writer.writerows(self.results)
        columns = ['part', 'test', 'scope', 'status', 'seconds', 'useful_bytes', 'useful_MiB_s',
                   'server_stream_bytes', 'client_stream_bytes', 'cuts', 'connections', 'note']
        with (self.directory / 'measurements.csv').open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=columns)
            writer.writeheader()
            writer.writerows(self.measurements)
        print('\n' + summary, flush=True)
        return exit_code


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('root', type=Path, nargs='?', default=Path('.'))
    parser.add_argument('dataset', type=Path, nargs='?')
    parser.add_argument('--only', choices=PARTS, action='append', help='repeat to select parts; otherwise all')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--robustness', action='store_true', help='include non-mandatory edge/transport checks')
    parser.add_argument('--clients', type=int, default=None)
    parser.add_argument('--timeout', type=float, default=180, help='seconds per client/batch; finite operational budget')
    parser.add_argument('--startup-timeout', type=float, default=10)
    parser.add_argument('--build-timeout', type=float, default=120)
    parser.add_argument('--fault-prob', type=float, action='append', default=None,
                        help='native random probability; repeat; defaults .0001 and .001, not universal completion tests')
    parser.add_argument('--fault-trials', type=int, default=2)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--adapter', type=Path)
    parser.add_argument('--results-dir', type=Path)
    parser.add_argument('--keep-output', action='store_true')
    parser.add_argument('--submission', action='store_true', help='also enforce extracted submission root name')
    parser.add_argument('--group', nargs=2, metavar=('XXX', 'YYY'))
    parser.add_argument('--archive', type=Path, help='inspect final ZIP/tar.gz safely without extracting/running it')
    parser.add_argument('--require-complete', action='store_true', help='exit 3 if required evidence is missing')
    args = parser.parse_args(argv)
    args.clients = args.clients if args.clients is not None else (4 if args.quick else 12)
    args.fault_prob = args.fault_prob if args.fault_prob is not None else [0.0001, 0.001]
    if not 2 <= args.clients <= 128:
        parser.error('--clients must be in 2..128')
    if not 1 <= args.fault_trials <= 100:
        parser.error('--fault-trials must be in 1..100')
    if any(not math.isfinite(x) or x <= 0 for x in (args.timeout, args.startup_timeout, args.build_timeout)):
        parser.error('timeouts must be finite and positive')
    if any(not math.isfinite(p) or not 0 < p < 1 for p in args.fault_prob):
        parser.error('diagnostic probabilities must be strictly between 0 and 1; p=0 is always tested')
    if args.group and any(not re.fullmatch(r'\d{3}', x) for x in args.group):
        parser.error('--group requires two zero-padded three-digit group numbers')
    if not args.root.is_dir():
        parser.error('root must be an existing submission/repository directory')
    return args


def main(argv=None):
    args = arguments(argv)
    if not sys.platform.startswith('linux'):
        print('This tester runs only on Linux. Use a GitHub Codespace or Linux host. No builds/tests started.', file=sys.stderr)
        return 2
    if not args.no_build and shutil.which('make') is None:
        print('make is missing; install build-essential or supply already built programs with --no-build.', file=sys.stderr)
        return 2
    suite = None
    interrupted = False
    try:
        suite = Suite(args)
        print('Preliminary IT305 evaluation. Report directory: ' + str(suite.directory), flush=True)
        suite.execute()
    except KeyboardInterrupt:
        interrupted = True
    except Exception as e:
        if suite:
            suite.record('evaluator', 'fatal error', 'ERROR', traceback.format_exc())
        else:
            print('Evaluator setup failed: %s: %s' % (type(e).__name__, e), file=sys.stderr)
            return 2
    finally:
        if suite:
            suite.run.close()
    if suite:
        code = suite.save(interrupted)
        if not args.keep_output:
            remove_owned(suite.work, suite.directory)
        return code
    return 130 if interrupted else 2


if __name__ == '__main__':
    sys.exit(main())
