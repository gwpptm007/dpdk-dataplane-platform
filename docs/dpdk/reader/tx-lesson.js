"use strict";

/** 固定一条合法的 ixgbe simple 教学路径，避免把不同驱动的回收策略混成同一流程 */
const txSteps = [
  {
    name: "应用持有", actor: "应用 worker",
    title: "先确认谁有权使用这 32 个包",
    text: "假设有 32 个普通直接 mbuf，每个只有一段，引用计数为 1，没有 TX 卸载请求，且都来自同一个池。每个包的字节和长度已经准备好。数组中保存的是 mbuf 指针，实际数据仍在各自的数据区。示例池总计 64 个对象，此时另有 32 个空闲。",
    changed: "本批对象由应用持有；TX 描述符环已初始化但尚未提交本批",
    owner: "应用可以读取、修改或主动丢弃这 32 个包",
    why: "TX 不会替应用重新推断包的所有含义。数据地址、长度与卸载元数据必须正确；如果还准备修改某个包，就不能同时把同一份数据交给可能开始 DMA 的设备。",
    app: "持有 32 个包", sw: "本批尚未进入软件记录", data: "32 个数据区 · 内容已准备",
    desc: "本例空环 · 128 个槽位", flags: "simple 初始可用数为 127", nic: "尚未得到本批边界",
    pool: "空闲 32 / 64 个对象", reclaim: "本批还没有待回收对象", active: ["app", "data"], edge: []
  },
  {
    name: "检查空间", actor: "应用线程中执行的 TX PMD",
    title: "先算描述符够不够，再决定接受多少包",
    text: "应用调用 rte_eth_tx_burst，PMD 进入 simple 发送路径。本例 tx_rs_thresh 和 tx_free_thresh 都为 32，空闲描述符为 127，足以接受本批 32 个单段包。真实调用若空闲量低于回收触发阈值，会先尝试清理旧的完成组；没有完成依据时仍可能只接受一部分。",
    changed: "决定本次可接受数量；检查的是描述符资源，并非对端是否愿意接收",
    owner: "调用尚在执行，应用不能从另一线程同时改动本批数据",
    why: "RX 需要新的空对象接包，TX 已经拿到了装有数据的对象；TX 此时主要还需要描述符位置。空闲 mbuf 多，并不直接意味着 TX 环里有空位。",
    app: "正在调用 TX burst", sw: "检查 127 个可用位置", data: "调用期间保持数据稳定",
    desc: "本批需要 32 个数据描述符", flags: "不涉及多段或 context", nic: "尚未发布本批",
    pool: "空闲仍是 32 / 64", reclaim: "检查条件 ≠ 已有完成结果", active: ["sw", "desc"], edge: ["submit"]
  },
  {
    name: "填写描述符", actor: "CPU / PMD",
    title: "硬件拿地址，软件留下回收线索",
    text: "PMD 把 32 个 mbuf 的指针保存在软件环中，并把数据区的 IOVA、长度和发送命令写入描述符 0～31。这里每个包只有一段，因此每个数据描述符都带包结束标记 EOP。第 31 号位置设置 RS，请求设备为这一组报告完成；其他位置不必各自写回一个 DD。",
    changed: "描述符中有设备可读地址；软件环中有之后要释放的 mbuf 指针",
    owner: "仍在本次调用内；PMD 正在准备提交，应用不能并发改写",
    why: "网卡按地址读字节，PMD 按软件记录找回对象。RS 是完成报告分组的选择，不是要求先等到 32 个包才允许发送；一批只有几个包也可以更新门铃提交。",
    app: "仍在同一次函数调用中", sw: "保存 mbuf[0]…mbuf[31]", data: "地址与长度被描述符引用",
    desc: "槽位 0～31 已写好", flags: "逐包 EOP · 最后位置有 RS", nic: "还未通过门铃发布本批",
    pool: "空闲仍是 32 / 64", reclaim: "RS 请求报告，不是完成结果", active: ["sw", "desc"], edge: ["describe"]
  },
  {
    name: "发布并返回", actor: "CPU / PMD 发布，应用收到返回值",
    title: "门铃划出设备可消费范围，返回值划出应用责任边界",
    text: "所选路径在写好描述符后执行 rte_wmb，再把新尾位置 32 写入 TDT。TDT 是发送描述符尾寄存器，这次门铃通知 NIC 有新的工作可取。函数返回 32，于是应用不再访问这 32 个 mbuf。本图为了便于观察，把 DMA 画在下一步；硬件实际可以从门铃发布后就并行工作，无需等函数返回。",
    changed: "本批发布给设备；这次提交后 nb_tx_free 为 95；返回接受数量 32",
    owner: "本批全部交给发送路径，应用不能读写或再次 free",
    why: "写屏障约束相关内存写入与设备通知的顺序，并不等待网络传输结束。函数已经返回，是软件调用结束；数据生命周期还没有结束。",
    app: "返回 32 · 本批不可再访问", sw: "记录仍保留，供以后回收", data: "发送路径持有 32 个包",
    desc: "TDT = 32 · 已发布 0～31", flags: "本例可用描述符还剩 95", nic: "可以开始消费本批",
    pool: "空闲仍是 32 / 64", reclaim: "接受了 32，不等于回收了 32", active: ["app", "desc", "nic"], edge: ["publish"]
  },
  {
    name: "DMA读取", actor: "NIC 的 DMA 引擎与发送逻辑",
    title: "这次数据流向与 RX 相反",
    text: "NIC 根据描述符里的地址读取主机数据，再由设备发送逻辑处理网络帧。RX 是设备把字节写进接收缓冲，TX 则是设备读取已经准备好的发送缓冲。我们选择一个尚未出现可回收完成报告的观察时刻，软件仍必须保留这些数据。",
    changed: "NIC 在读取数据、处理本地发送；本例尚未观察到该完成组的 DD",
    owner: "应用仍不能改写或释放，PMD 也不能仅凭时间过去就当作安全",
    why: "若应用提前归还，别的路径可能把同一块内存拿去接收新包。网卡随后读到的可能就是被覆盖的内容。能否回收必须有具体完成协议，不能靠“应该已经发完了”。",
    app: "已失去本批使用权", sw: "保留指针等待完成依据", data: "NIC DMA 从这些区域读取",
    desc: "等待对应完成组结果", flags: "暂未观察到目标 DD", nic: "读取字节并进行本地发送",
    pool: "空闲仍是 32 / 64", reclaim: "本地处理过程不等于远端确认", active: ["data", "nic"], edge: ["dma"]
  },
  {
    name: "完成写回", actor: "NIC 写回状态，PMD 尚未执行本组回收",
    title: "可安全回收，和已经归还，是两件事",
    text: "当所选设备协议允许回收这一组缓冲时，RS 对应位置的 DD 提供本地完成依据。本例第 31 号描述符已经给出所需结果。可是 PMD 还没有执行释放，软件记录和对象仍然占着。这里没有新发包触发普通清理，池里仍只有原来的 32 个空闲对象。",
    changed: "完成报告已经可见；32 个已完成对象仍被发送路径持有",
    owner: "具备回收条件，不代表应用重新拿回了这些指针的使用权",
    why: "设备只能报告自己的工作状态，不能替应用执行 DPDK 的引用计数和内存池操作。只有软件真的消费完成信息并释放对象，资源才进入下一轮可分配状态。DD 也不是 TCP ACK 或对端应用处理回执。",
    app: "仍不可读取旧指针", sw: "32 个指针仍待处理", data: "本地完成 · 尚未归还",
    desc: "第 31 号位置 DD 已就绪", flags: "本例可回收这一组 32 个", nic: "已提供本地完成依据",
    pool: "空闲仍是 32 / 64", reclaim: "完成 32 个，池余量仍未变化", active: ["desc", "nic"], edge: ["complete"]
  },
  {
    name: "驱动回收", actor: "CPU / PMD 的清理路径",
    title: "软件取走完成结果，资源循环才真正闭合",
    text: "假设维护逻辑现在独占该 TX 队列，调用此路径支持的 rte_eth_tx_done_cleanup(port, queue, 0)。simple 清理代码检查完成组，释放相应 mbuf 引用，并把可归还对象交回池。本例 32 个对象都满足直接、单段、独占条件，因此清理返回 32，可用描述符恢复为 127。这个主动调用仅用于示范，本项目目前没有直接调用该 API。",
    changed: "发送路径不再持有本批；池中 64 个对象都可复用，本例忽略本核缓存",
    owner: "对象回到池；应用将来必须重新分配，不能复活保存下来的旧指针",
    why: "这个结果依赖完整完成组、驱动支持和正确的队列使用方式。cleanup 不会等待或强制设备完成；少量尾包不足 RS 组时也不保证清空。正常热路径还可以在空闲描述符不足时触发同类回收。",
    app: "需要对象时重新分配", sw: "本组指针不再承担持有责任", data: "归还池 · 旧内容可能还在",
    desc: "可用描述符恢复为 127", flags: "回收是软件动作", nic: "完成状态已被软件消费",
    pool: "空闲恢复为 64 / 64", reclaim: "下一轮可接收、处理或发送", active: ["sw", "pool"], edge: ["reclaim"]
  }
];

/** 只高亮本步真实关注的关系，箭头不代表总线拓扑或耗时比例 */
function txDiagram(step, unique) {
  const box = (id, x, y, width, title, line1, line2) => `<g class="${step.active.includes(id) ? "active" : ""}">
    <rect class="box" x="${x}" y="${y}" width="${width}" height="88" rx="6"/>
    <text class="label" x="${x + 14}" y="${y + 27}">${title}</text>
    <text x="${x + 14}" y="${y + 50}">${line1}</text>
    <text class="detail" x="${x + 14}" y="${y + 71}">${line2}</text></g>`;
  const arrow = (id, d) => step.edge.includes(id)
    ? `<path class="flow hot" d="${d}" marker-end="url(#tx-arrow-${unique})"/>` : "";
  return `<div class="diagram-scroll" tabindex="0" role="region" aria-label="可横向滚动的 TX 状态图">
    <svg class="state-art" viewBox="0 0 960 450" role="img" aria-labelledby="tx-title-${unique} tx-desc-${unique}">
      <title id="tx-title-${unique}">${step.name}时的发送状态</title>
      <desc id="tx-desc-${unique}">${step.changed}。${step.owner}</desc>
      <defs><marker id="tx-arrow-${unique}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M 0 0 L 10 5 L 0 10 z" fill="#006d67"/></marker></defs>
      <text class="domain" x="20" y="25">固定示例 · 单段无卸载 · 32 个包是一组 · 省略其他流量和缓存</text>
      ${box("app", 20, 50, 280, "应用 worker", step.app, "只使用自己仍持有的对象")}
      ${box("sw", 340, 50, 280, "PMD 软件环", step.sw, "供回收使用的 mbuf 指针记录")}
      ${box("data", 660, 50, 280, "报文数据区", step.data, "报文字节不装进描述符里")}
      ${box("pool", 20, 205, 280, "mempool", step.pool, "已分配对象不计入这里的空闲量")}
      ${box("desc", 340, 205, 280, "TX 描述符环", step.desc, step.flags)}
      ${box("nic", 660, 205, 280, "NIC / DMA", step.nic, "本地完成不证明对端收到")}
      ${arrow("submit", "M 305 94 H 330")}
      ${arrow("describe", "M 480 148 V 195")}
      ${arrow("publish", "M 630 235 H 650")}
      ${arrow("dma", "M 800 148 V 195")}
      ${arrow("complete", "M 650 265 H 630")}
      ${arrow("reclaim", "M 355 148 V 175 H 160 V 195")}
      <rect x="20" y="340" width="920" height="80" rx="6" fill="#edf3ec"/>
      <text class="label" x="40" y="370">当前最需要区分的事</text>
      <text x="40" y="398">${step.reclaim}</text>
    </svg></div>`;
}
DpdkReader.mountSteps(txSteps, txDiagram);

/** 这个练习只演示 API 前缀所有权，不把五个包映射成真实 ixgbe 的 RS 分组 */
(() => {
  const accepted = document.getElementById("accepted");
  const submit = document.getElementById("submit-tx");
  const complete = document.getElementById("complete-tx");
  const reclaim = document.getElementById("reclaim-tx");
  const status = document.getElementById("exercise-status");
  let phase = "initial";
  let count = 0;

  function valid() {
    return accepted.value !== "" && accepted.validity.valid;
  }

  /** 已完成数量只是发送路径占用量的一部分，不能再次计入对象总数 */
  function render() {
    const app = phase === "initial" ? 5 : 0;
    const driver = phase === "submitted" || phase === "completed" ? count : 0;
    const done = phase === "completed" ? count : 0;
    document.getElementById("app-count").textContent = app;
    document.getElementById("driver-count").textContent = driver;
    document.getElementById("done-count").textContent = done;
    document.getElementById("pool-count").textContent = 8 - app - driver;
    accepted.disabled = phase !== "initial";
    submit.disabled = phase !== "initial" || !valid();
    complete.disabled = phase !== "submitted" || count === 0;
    reclaim.disabled = phase !== "completed";

    const names = ["A", "B", "C", "D", "E"];
    document.getElementById("packet-cards").innerHTML = names.map((name, index) => {
      const owner = phase === "initial" ? "应用持有"
        : index >= count || phase === "reclaimed" ? "已归还池"
          : phase === "completed" ? "已完成，待回收" : "发送路径持有";
      const style = owner === "应用持有" ? "app" : owner === "已归还池" ? "pool" : "driver";
      return `<div class="packet-card ${style}"><strong>${name}</strong><span>${owner}</span></div>`;
    }).join("");
    if (phase === "initial") {
      status.textContent = valid() ? "应用持有 A～E，池中另有 3 个对象。先选择 TX 返回值，再提交"
        : "返回值必须是 0～5 的整数，请修改后再提交";
    } else if (phase === "submitted") {
      status.textContent = count === 0 ? "返回 0：本项目释放全部未接受对象，池余量恢复为 8，无本批发送对象可完成"
        : `返回 ${count}：前 ${count} 个交给发送路径；本项目立即释放剩余 ${5 - count} 个。下一步观察完成与归还的区别`;
    } else if (phase === "completed") {
      status.textContent = `${count} 个已接受对象现在满足本地回收条件，但仍由发送路径持有。仅报告完成不会增加池余量`;
    } else {
      status.textContent = `驱动实际释放 ${count} 个对象后，池余量才恢复为 8。没有真实 DMA、RS 分组或硬件时钟参与本练习`;
    }
  }

  accepted.addEventListener("input", render);
  submit.addEventListener("click", () => {
    if (phase !== "initial" || !valid()) return;
    count = Number(accepted.value);
    phase = "submitted";
    render();
  });
  complete.addEventListener("click", () => {
    if (phase !== "submitted" || count === 0) return;
    phase = "completed";
    render();
  });
  reclaim.addEventListener("click", () => {
    if (phase !== "completed") return;
    phase = "reclaimed";
    render();
  });
  document.getElementById("reset-tx").addEventListener("click", () => {
    phase = "initial";
    count = 0;
    accepted.value = "3";
    render();
  });
  render();
})();
