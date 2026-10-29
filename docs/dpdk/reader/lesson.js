"use strict";

/** 这些状态只演示一个单段包的因果顺序，不驱动真实网卡或预测处理耗时 */
const rxSteps = [
  {
    name: "准备对象", actor: "CPU / PMD 的启动代码",
    title: "先有能接包的内存，才有地方放新包",
    text: "池里预先创建了 mbuf 和数据区。A、B 是两个可复用对象，RX 描述符另在一块内存中。图只展开一个槽位；真正的 RX 队列有很多槽位。此时网卡还没有得到这个槽位对应的有效接收缓冲。",
    changed: "建立 pool、描述符环和软件配套数组；尚未把 A 提供给设备",
    owner: "A、B 都在池中；应用还没有任何报文可读",
    why: "如果收包时才临时寻找内存，网卡到达的数据就没有稳定的目的地。预分配把容量规划和地址准备放在热路径之前，但运行时仍要不断从池中拿替补对象。",
    descriptor: "尚未提供数据区地址", status: "本槽位未投入接收", a: "在池中 · 无有效报文", b: "在池中 · 无有效报文",
    cpu: "建立队列与内存池", wire: "尚未接收本包", active: ["pool", "cpu"], paths: []
  },
  {
    name: "挂接到 RX", actor: "CPU / PMD 的队列初始化",
    title: "给网卡地址，给软件保留查找线索",
    text: "PMD 取出 A，把 A 数据区的设备地址写入描述符，并在软件配套数组中记录“这个槽位对应 A”。准备好的槽位按设备协议发布给 NIC。描述符给硬件看，软件指针给 CPU 看，二者回答的是不同的问题。",
    changed: "描述符指向 A 的数据区；sw_ring[槽位] 指向 mbuf A",
    owner: "A 留给 NIC 接收；B 仍在池中；应用不能提前读写 A",
    why: "NIC 只需要知道字节写到哪里，不必理解 C 结构体的全部字段。等完成事件出现，PMD 用同一槽位索引找回 A，再补齐应用需要的元数据。",
    descriptor: "接收地址 = A 的数据 IOVA", status: "DD = 0 · 已发布给 NIC", a: "已挂 RX · 等待 NIC 写入", b: "在池中 · 可作替补",
    cpu: "记录 sw_ring[槽位] = A", wire: "等待报文", active: ["descriptor", "a"], paths: ["dma"]
  },
  {
    name: "DMA 搬运", actor: "NIC 的 DMA 引擎",
    title: "字节在移动，CPU 没有执行逐字节复制",
    text: "假设这个包交付给 PMD 的长度为 96 字节，且一个数据区足够容纳。NIC 按已提供的 DMA 地址，把收到的字节写入 A 的数据区。图把 DMA 与完成拆成两步，是为了看清因果关系；现实中这些是流水化的硬件操作。",
    changed: "A 的数据区正在被写入；描述符尚未报告完成",
    owner: "NIC 仍在生产 A 的内容；CPU 不能把看到的部分字节当成完整包",
    why: "有些字节已经出现在内存里，不等于接收已完成。没有完成协议，CPU 可能读到半个包、旧长度，或前一次使用留下的内容。",
    descriptor: "接收地址 = A 的数据 IOVA", status: "DD = 0 · 正在接收", a: "NIC 正在写入 96 字节", b: "在池中 · 可作替补",
    cpu: "轮询完成信息，尚不交付", wire: "DMA → A 的数据区", active: ["nic", "a"], paths: ["dma"]
  },
  {
    name: "完成写回", actor: "NIC 写回完成信息；CPU / PMD 检查它",
    title: "完成标志是一道交接关口",
    text: "NIC 写回完成标志和接收结果。以 ixgbe 标量路径为例，PMD 先检查 DD，再执行 acquire fence，然后读取描述符快照。正确性来自设备协议、DMA 映射与内存顺序的共同配合，不能把这一整套机制缩写成“加 volatile 就好了”。",
    changed: "描述符处于完成状态，可读取设备报告的长度等信息",
    owner: "A 已完成接收，但还没有交给应用；本槽位仍未补充新缓冲",
    why: "看到 DD 只解决“这次接收完成了吗”。还必须解决“下一个包往哪里写”和“怎样把 A 交给调用者”。完成、补充、交付是三个事件。",
    descriptor: "完成结果 · 长度等元数据", status: "DD = 1 · 等待 PMD 消费", a: "96 字节已接收 · 未交付", b: "在池中 · 可作替补",
    cpu: "检查 DD → 有序读取结果", wire: "完成信息写回描述符", active: ["descriptor", "cpu"], paths: ["complete", "inspect"]
  },
  {
    name: "换缓冲", actor: "CPU / PMD 的 ixgbe_recv_pkts 路径",
    title: "把 A 留给应用，把 B 留给下一包",
    text: "这条驱动路径先从池中申请 B，成功后再前移扫描位置。它用 B 替换槽位的软件记录和接收地址，为 A 填好长度等 mbuf 元数据，把 A 放进本次返回数组。此时 A 的 96 字节仍在原处，没有因为返回指针而再复制一遍。",
    changed: "sw_ring[槽位] = B；描述符改填 B 的地址；rx_pkts 中准备交付 A",
    owner: "A 在 PMD 待返回数组中；B 已从池取走；新描述符还需要按批量阈值通知设备",
    why: "替补不能是仍要交给应用的 A，否则新包可能覆盖应用正在解析的旧包。若申请 B 失败，这条路径保留当前完成包并停止本轮扫描，见下面的失败分支。",
    descriptor: "接收地址 = B 的数据 IOVA", status: "新条目已写好 · 等待发布", a: "PMD 已填元数据 · 待返回", b: "替换 A · 已挂到 RX 槽位",
    cpu: "rx_pkts 中准备 A 的指针", wire: "本槽位等待重新发布", active: ["descriptor", "a", "b", "cpu"], paths: ["replace"]
  },
  {
    name: "burst 返回", actor: "CPU / PMD → 应用 worker",
    title: "返回的是包指针，补充的是下一轮接收能力",
    text: "假设本轮还处理了后续其他槽位，累计补充量满足发布条件，更新后的 RDT 尾边界已经覆盖 B。PMD 更新寄存器，随后返回实际包数，返回数组中包含 A。若没达到发布阈值，可以先返回 A，再在之后更新门铃。仅仅发生一次 RDT 写入，也不保证最新替补立刻进入设备可消费的范围。",
    changed: "本例发布范围已包含 B；函数返回的 n 个包中包含 A，其他槽位未画出",
    owner: "A 交给应用读写或转交；B 归 RX 接收流程管理，二者数据区不同",
    why: "nb_pkts 是可返回的上限，不是必须等到的数量。batch 能摊薄固定开销，但调用者仍须只处理返回值指定的数组前缀，并承担最终转交或释放的责任。",
    descriptor: "接收地址 = B 的数据 IOVA", status: "本例已更新 RDT · 可接下一包", a: "应用持有 · 96 字节有效包", b: "NIC 可用 · 等待下一包",
    cpu: "rte_eth_rx_burst 返回 n", wire: "后续报文可以写入 B", active: ["a", "cpu", "nic"], paths: ["deliver", "nextdma"]
  },
  {
    name: "归还资源", actor: "应用，或完成发送后的 TX PMD",
    title: "结束一次使用，不等于删除这块内存",
    text: "假设应用丢弃这个包，且 A 为引用计数 1 的普通直接 mbuf，rte_pktmbuf_free(A) 会让它回到可复用资源中。缓冲池可能先收进当前 lcore 的缓存，不能保证 A 立刻成为另一核能取到的下一个对象。释放后应用不能继续访问 A。",
    changed: "A 的本次应用生命周期结束，原数据字节不需要被清零",
    owner: "A 可再次被分配；B 仍挂在 RX 上。下一次收包不保证恰好用回 A",
    why: "若应用选择发送，TX burst 已接受的前缀应由驱动回收，应用不能再 free；没被接受的尾部仍由应用处理。生命周期是否闭合，比 malloc/free 调用了几次更有意义。",
    descriptor: "接收地址 = B 的数据 IOVA", status: "继续提供 RX 接收能力", a: "归还池 / 本核缓存 · 不可再访问", b: "NIC 可用 · 等待下一包",
    cpu: "结束对 A 的使用权", wire: "接收流程继续", active: ["pool", "a"], paths: ["recycle"]
  }
];

/** 图只画角色与状态，位置和箭头不表示总线布局、执行时间或实际指针值 */
function diagram(step, unique) {
  const active = id => step.active.includes(id) ? "active" : "";
  const edge = (id, d) => `<path class="flow ${step.paths.includes(id) ? "hot" : ""}" d="${d}" marker-end="url(#arrow-${unique})"/>`;
  const box = (id, x, y, w, title, line1, line2) => `<g class="${active(id)}">
    <rect class="box" x="${x}" y="${y}" width="${w}" height="83" rx="6"/>
    <text class="label" x="${x + 14}" y="${y + 26}">${title}</text>
    <text x="${x + 14}" y="${y + 48}">${line1}</text>
    <text class="detail" x="${x + 14}" y="${y + 68}">${line2}</text></g>`;
  return `<div class="diagram-scroll" tabindex="0" role="region" aria-label="可横向滚动的状态图">
  <svg class="state-art" viewBox="0 0 920 410" role="img" aria-labelledby="title-${unique} desc-${unique}">
    <title id="title-${unique}">${step.name}时的接收状态</title>
    <desc id="desc-${unique}">${step.changed}。${step.owner}。高亮框表示本步重点变化</desc>
    <defs><marker id="arrow-${unique}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#5b8878"/></marker></defs>
    <rect x="220" y="10" width="690" height="390" rx="10" fill="#edf3ec"/>
    <text class="domain" x="238" y="32">主机侧 · 元数据与数据区是不同对象</text>
    ${box("nic", 10, 70, 190, "NIC / DMA", "接收网络帧", step.wire)}
    ${box("descriptor", 240, 58, 320, "RX 描述符 · 一个槽位", step.descriptor, step.status)}
    ${box("cpu", 600, 58, 290, "CPU · PMD / 应用", step.cpu, "由软件解释和管理 mbuf")}
    ${box("a", 240, 195, 320, "mbuf A ＋ A 的数据区", step.a, "mbuf 元数据与报文字节分开理解")}
    ${box("b", 600, 195, 290, "mbuf B ＋ B 的数据区", step.b, "与 A 是不同缓冲")}
    ${box("pool", 310, 310, 510, "mempool / cache", "重复分配和归还对象", "A、B 的分配状态见各自框内，不代表都在池中")}
    ${edge("dma", "M 105 153 V 236 H 230")}
    ${edge("complete", "M 200 96 H 230")}
    ${edge("inspect", "M 570 96 H 590")}
    ${edge("replace", "M 745 185 V 166 H 400 V 151")}
    ${edge("deliver", "M 570 236 H 580 V 112 H 590")}
    ${edge("nextdma", "M 105 153 V 294 H 745 V 288")}
    ${edge("recycle", "M 400 288 V 300")}
    <text class="detail" x="12" y="335">深色线：本步关联</text>
    <text class="detail" x="12" y="358">箭头：写入或交接</text>
    <text class="detail" x="12" y="381">详细含义见下方文字</text>
  </svg></div>`;
}

DpdkReader.mountSteps(rxSteps, diagram);

/** 这是守恒关系练习，省略多段、TX 和缓存占用后只计算一换一补充的理论余量 */
function updateBudget() {
  const inputs = ["pool-size", "rx-slots", "held"].map(id => document.getElementById(id));
  const result = document.getElementById("budget-result");
  if (inputs.some(input => input.value === "" || !input.validity.valid)) {
    result.textContent = "请输入范围内的整数，再比较资源占用";
    result.classList.add("invalid");
    return;
  }
  const [total, posted, held] = inputs.map(input => Number(input.value));
  const available = total - posted - held;
  result.classList.toggle("invalid", available <= 0);
  if (available < 0) {
    result.textContent = `这个状态不成立：占用比总量多 ${-available} 个对象。必须先减少占用或扩大池`;
  } else if (available === 0) {
    result.textContent = "替补余量为 0。已挂到 RX 的缓冲仍存在，但下一次申请替补可能失败；这不是“所有新包立刻全部丢弃”";
  } else {
    result.textContent = `替补余量 ${available} 个。在完全不归还对象的假设下，最多再完成 ${available} 次单段包的一换一补充；这不是吞吐量或丢包时刻预测`;
  }
}
["pool-size", "rx-slots", "held"].forEach(id => document.getElementById(id).addEventListener("input", updateBudget));
updateBudget();
