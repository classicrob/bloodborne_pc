# Opt-in guest fault dumps

`BB_FAULT_DUMP=1` enables the diagnostic for fatal guest SIGSEGV/SIGBUS on Apple. Unset, empty or `0` disables it. GPU write-tracking recovery and speculative-read recovery run first and retain their existing behavior, even with dumping enabled.

The dump contains saved RAX/RBX/RCX/RDX/RSI/RDI/R8–R15, RIP, RSP, RBP and RFLAGS. Register values in the 32 GiB–1 TiB guest-address range receive a 128-byte memory window, beginning 64 bytes before the value. This includes unavailable guest addresses: unreadable rows are reported rather than skipped silently. The stack receives an additional 256-byte dump starting at RSP.

Extra addresses can be requested instead of `1`:

```sh
BB_FAULT_DUMP='r9,[r9],[[r9]+0x38],r11,r14+0xb3c0,rbp,rsp,rcx' \
BB_PREBUILT=1 bash run.sh
```

Each extra expression dumps 128 bytes **starting at** its evaluated address. `[r9]` safely reads the 64-bit pointer stored at R9, then dumps the pointed-to resource. `[[r9]+0x38]` reads that resource's pointer at byte offset 0x38, then dumps its target. This covers the movie-fault resource's data (+0x08), flags (+0x23), allocator (+0x30) and table (+0x38), the context's list header, and the table's 16 u16 entries.

Expressions accept saved register names (case insensitive), the `image` base, unsigned decimal/hex offsets with `+` or `-`, and up to three nested brackets. For example, `image+0x56a63c0` selects the global context array. Up to 16 expressions of fewer than 64 characters each are accepted. Malformed configuration fails at startup; address overflow or an unreadable dereference is reported at fault time.

Configuration is parsed once before handlers are installed. The added diagnostic does no work on ordinary execution or recovered faults. Its fatal path uses fixed buffers, `write(2)` and `mach_vm_read_overwrite`; it performs no allocation, printf formatting, symbol lookup or C dereference of guest memory. Mach read failures are logged with the kernel return code, 16 bytes at a time, so a window crossing an unavailable page still shows readable rows. The opt-in guest path replaces the legacy symbolized fatal report and exits with the original `128 + signal` status. Other platforms and dump-off reporting are unchanged.

`tests/test_fault_mac.c`, already part of Darwin `build.sh --test`, includes generated low guest code that faults deliberately. It checks saved registers, resource and nested-table reads, inaccessible memory, overflow/underflow, configuration errors, dump-off behavior and the original recovery paths with the option enabled. No game changes are required.
