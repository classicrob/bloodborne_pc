#!/usr/bin/env python3
from pathlib import Path
import resource
import subprocess
import sys

output = Path(sys.argv[1]).resolve()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
for op in (0, 1, 2):
    for rounding in range(4):
        result = subprocess.run(
            ["arch", "-x86_64", str(output / "emit"), str(output), str(rounding), str(op), "1"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if op == 0 and rounding != 0:
            assert result.returncode != 0 and "V_CVT_F16_F32 rounding mode" in result.stderr, result
        else:
            assert result.returncode == 0, result
            subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(output / "policy.spv")], check=True)
for op in (0, 1):
    result = subprocess.run(
        ["arch", "-x86_64", str(output / "emit"), str(output), "0", str(op), "0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode != 0 and "requires native Float16" in result.stderr, result
print("F16 policy: RTE narrowing; exact widening and bitcasts under all rounding modes; missing capability loud PASS")

for mode in range(4):
    for op in (0, 1, 2):
        for rounding in range(4):
            path = output / "entry.spv"
            result = subprocess.run(
                ["arch", "-x86_64", str(output / "entry"), str(path), str(mode), str(rounding), str(op)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            if op == 0 and rounding != 0:
                assert result.returncode != 0 and "V_CVT_F16_F32 rounding mode" in result.stderr, result
            else:
                assert result.returncode == 0, result
                assert "Float32 and Float16/64" not in result.stderr, result
                subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(path)], check=True)
print("Full EmitSPIRV half-mode policy: 48 conversion/bitcast controls PASS")
