# Build and run

## 1. Mock mode (validated in current container)

```bash
./scripts/build.sh
./scripts/run_platform.sh --port 0 --rxq 1 --txq 1 --burst 32 --loops 1
```

## 2. Real DPDK mode (to be validated on target host)

```bash
DPPD_BUILD_MODE=dpdk ./scripts/build.sh
./build/dppd -l 0-1 -n 4 -- --port 0 --rxq 1 --txq 1 --burst 32 --loops 1000
```

## Notes

- Arguments before `--` belong to DPDK EAL.
- Arguments after `--` belong to the `dppd` application.
- Current Phase 1 behavior focuses on:
  - receive packet
  - parse Ethernet / ARP / IPv4 / UDP / TCP
  - allow ARP and UDP baseline path
  - run minimal ACL / route / NAT / rewrite logic
  - transmit packet back through the same port/queue
