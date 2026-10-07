"""Generate legal comparison variants; never request unsupported denorm modes."""
from pathlib import Path
import subprocess
out = Path('out/mac-float-modes')
subprocess.run(['spirv-dis', str(out/'float.spv'), '-o', str(out/'float.spvasm')], check=True)
subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(out/'float.spv')], check=True)
plain = (out/'float.spvasm').read_text()
assert 'NoContraction' in plain and ' Fma ' in plain
preserve = plain.replace('OpCapability Shader', 'OpCapability Shader\nOpCapability SignedZeroInfNanPreserve', 1)
preserve = preserve.replace('OpExecutionMode %main LocalSize 1 1 1',
                            'OpExecutionMode %main LocalSize 1 1 1\nOpExecutionMode %main SignedZeroInfNanPreserve 32', 1)
for name, assembly in [('preserve', preserve), ('allow-contraction', '\n'.join(
        line for line in preserve.splitlines() if 'NoContraction' not in line))]:
    (out/f'{name}.spvasm').write_text(assembly+'\n')
    subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', str(out/f'{name}.spvasm'),
                    '-o', str(out/f'{name}.spv')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', str(out/f'{name}.spv')], check=True)
