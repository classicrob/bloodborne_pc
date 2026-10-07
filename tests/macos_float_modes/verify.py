"""Check raw production results, without accepting unrelated float-mode gaps."""
from pathlib import Path
import os
import re
import statistics

out = Path('out/mac-float-modes')
# Check the actual emitter shape with the workaround both disabled and enabled.
for variant, opcode in [('baseline', 'OpFMul'), ('production', 'Fma')]:
    assembly = (out/f'{variant}-bench.spvasm').read_text()
    assert len(re.findall(r'\b'+opcode+r'\b', assembly)) == 128
    assert assembly.count('NoContraction') == 128
    expected_integer_ops = 128 if variant == 'production' else 0
    assert assembly.count('OpBitwiseXor') == expected_integer_ops
    assert assembly.count('OpBitwiseAnd') == expected_integer_ops

expected = {}
for line in Path('tests/macos_float_modes/results-m4-27.2.tsv').read_text().splitlines():
    fields = line.split('\t')
    if len(fields) == 5 and re.fullmatch('[0-9a-f]{8}', fields[1]):
        expected[fields[0]] = int(fields[1], 16)
expected['mul_positive_zero_negative_one'] = 0x80000000
expected['mul_negative_zero_positive_one'] = 0x80000000
expected.update({
    'mul_negative_denormal': 0x80000000,
    'mul_negative_finite': 0xc0400000,
    'mul_overflow_positive': 0x7f800000,
    'mul_overflow_negative': 0xff800000,
    'mul_negative_inf': 0xff800000,
    'mul_zero_negative_inf': 0x7fc00000,
    'mul_quiet_nan': 0x7fc00000,
    'mul_positive_zeros': 0,
    'mul_double_negative_zero': 0,
    'fma_negative_zero_positive_addend': 0,
})
actual = {name: int(bits, 16) for name, bits in re.findall(
    r'^([a-z_0-9]+)\s+([0-9a-f]{8})$', (out/'production.log').read_text(), re.M)}
assert actual.keys() == expected.keys(), (actual.keys() - expected.keys(), expected.keys() - actual.keys())
for name, want in expected.items():
    got = actual[name]
    if (want & 0x7fffffff) > 0x7f800000:
        assert (got & 0x7fffffff) > 0x7f800000, (name, hex(got))
    else:
        assert got == want, (name, hex(got), hex(want))
print('Production emitter: 50 original + 10 supplemental cases PASS; signed-zero differences=0')

if os.environ.get('BB_FLOAT_BENCHMARK') == '1':
    groups = {'baseline': [], 'production': []}
    for i in range(6):
        text = (out/f'bench-{i}.log').read_text()
        assert '65,536 CPU/GPU bitwise results PASS' in text
        samples = re.findall(r'GPU_SAMPLE \S*/(baseline|production)-bench.spv (\d+) ([0-9.]+) ms', text)
        assert len(samples) == 24
        for variant, repetition, elapsed in samples:
            if int(repetition) >= 4:
                groups[variant].append(float(elapsed))
    for variant, samples in groups.items():
        assert len(samples) == 60 and min(samples) > 0
        q = statistics.quantiles(samples, n=4)
        print(f'{variant}: median {statistics.median(samples):.6f} ms, '
              f'IQR {q[0]:.6f}..{q[2]:.6f} ms, mean {statistics.mean(samples):.6f} ms '
              '(60 GPU timestamp samples, 8,388,608 multiplies per sample)')
    ratio = statistics.median(groups['production']) / statistics.median(groups['baseline'])
    print(f'Repaired/baseline median ratio {ratio:.4f} ({(ratio-1)*100:+.2f}%)')
