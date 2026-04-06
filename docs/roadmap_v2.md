# Roadmap V2

## Phase 1
- EAL / port / queue / worker 骨架跑通
- ARP / IPv4 / UDP / TCP parser 骨架
- route / ACL / NAT 占位
- checksum offload / ARP table / mbuf 学习样例

## Phase 2
- 多队列 / RSS / 多核 worker
- stats / pdump / Scapy 验证
- 更完整的软件 pipeline
- 补 examples

## Phase 3
- NAT aging / route lookup / ACL 深化
- regression / benchmark
- scenario docs 补全

## Phase 4
- `rte_flow_map.c`
- representor 基础
- transfer pipeline
- 对比 SW 与 HW_RTE 路径
