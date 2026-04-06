# Build verification

## What was verified in this package

This package was verified in the current container with:

```bash
./scripts/build.sh
./scripts/run_platform.sh --port 0 --rxq 1 --txq 1 --burst 32 --loops 1
```

## Result in current container

- repository builds successfully in **mock mode**
- binary generated at `build/dppd`
- mock mode now executes a **realistic Phase 1 call path**:
  - builds ARP + IPv4/UDP frames
  - runs real parser logic
  - runs software pipeline decision
  - runs baseline NAT/rewrite path
  - prints final stats

## Important note

Real DPDK link mode was **not verified in this container** because `libdpdk` is not installed here.

However, this package is no longer only a placeholder for DPDK mode: the code path for
`main.c`, `port_init.c`, `worker.c`, `parser.c`, and `pkt_rewrite.c` has already been
upgraded to a real DPDK Phase 1 baseline implementation.

The target host can validate it with:

```bash
DPPD_BUILD_MODE=dpdk ./scripts/build.sh
./build/dppd -l 0-1 -n 4 -- --port 0 --rxq 1 --txq 1 --burst 32 --loops 1000
```
