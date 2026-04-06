from scapy.all import Ether, ARP, IP, UDP, sendp

arp = Ether(dst='ff:ff:ff:ff:ff:ff') / ARP(pdst='10.0.0.2')
udp = Ether() / IP(dst='10.0.0.2') / UDP(dport=2152, sport=12345) / b'hello-dppd'

print('send ARP and UDP packets for basic validation')
# sendp([arp, udp], iface='eth1')
