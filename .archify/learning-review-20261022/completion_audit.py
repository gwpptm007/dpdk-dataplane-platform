import hashlib, json, pathlib, re
from source_audit import ROOT, OUT
source=json.loads((OUT/"source-check.json").read_text(encoding="utf-8"))
browser=json.loads((OUT/"browser-check.json").read_text(encoding="utf-8"))
manifest=json.loads((OUT/"manifest.json").read_text(encoding="utf-8"))
chapters=json.loads((OUT/"chapters.json").read_text(encoding="utf-8"))
assert source["passed"] and not source["noncomment_changes"]
new_steps=sum(len(c["steps"]) for c in chapters)
total_steps=17+new_steps
assert browser["passed"] and len(browser["pages"])==9 and sum(p["steps"] for p in browser["pages"])==total_steps
assert len(next(c for c in chapters if c["id"]=="2.1")["steps"])==16
assert not browser["runtimeErrors"]
for path,digest in browser["artifact_hashes"].items():
    assert hashlib.sha256((ROOT/path).read_bytes()).hexdigest()==digest,path
for name in browser["screenshots"]:assert (OUT/name).stat().st_size>1000
slot=(ROOT/"lib/control/slot_index.c").read_text(encoding="utf-8")
assert all(s in slot for s in ("删除 ID 20","随后新增 ID 25","空闲链变成 4 → 1 → NONE"))
for c in chapters:
    for s in c["steps"]:
        assert all(s.get(k) for k in ("title","input","output","explain","state","handoff","file","fn"))
    text=(OUT/("section-"+c["id"]+".html")).read_text(encoding="utf-8")
    assert "__STEPS__" not in text and "__TITLE__" not in text
assert len(manifest["source_files"])>=16
requirements=[
{"requirement":"索引删除与复用，同一具体例子及源码 plain text 注释","evidence":["lib/control/slot_index.c:dppd_slot_index_erase","section-2.3.html#step-6"],"result":"verified"},
{"requirement":"报文主线连贯覆盖收包、解析、规则、路由/ND、改写、发送释放","evidence":["1.1–1.3 existing pages","section-1.4.html","section-1.5.html","manifest.json"],"result":"verified by current source inspection and referenced excerpts"},
{"requirement":"规则主线覆盖控制请求、事务、匹配表、发布、账本、finalize、保存入口、删除回滚和QSBR","evidence":["section-2.1.html","section-2.2.html","section-2.3.html","section-2.4.html"],"result":"verified by current source inspection and referenced excerpts"},
{"requirement":"检查关键源码注释，补齐作用与原因，合并重复说明","evidence":["source-check.json changed_files","nd6 resolve purpose/return comment","control remove call sequence","pipeline and exact duplicate explanations consolidated"],"result":"verified"},
{"requirement":"沿用现有学习形式，前后可点击进入，输入输出代码状态下一步可见","evidence":["browser-check.json","slot-reuse.desktop.png","rewrite-send.desktop.png","learning-index.desktop.png"],"result":"verified; nine linked pages, all declared steps checked, existing sent and port controls preserved"},
{"requirement":"区分此前讲过、本次补充及后续待学，区分运行快照与磁盘文件","evidence":["index.html","section-2.3.html","section-2.4.html","review.html"],"result":"verified"},
{"requirement":"源码引用与本地打开效果核实","evidence":["manifest.json exact source spans","browser-check.json source anchors, chapter navigation, file URLs and 1440/800/390 widths"],"result":"verified; three representative screenshots visually inspected"},
{"requirement":"只改源码注释，保留任务开始时已有修改，不提交或push","evidence":["source-baseline.json","source-check.json"],"result":"verified against Goal-start workspace; no commit/push executed"},
{"requirement":"发现的问题留证据并保持范围，交付入口与下一阶段建议","evidence":["review.html static update-failure cleanup observation","index.html follow-up list"],"result":"verified; static concern recorded without runtime claim or fix"}
]
report={"complete":True,"requirements":requirements,"source_files_compared":source["source_count"],"modified_source_files":source["changed_files"],"pages":9,"steps":total_steps,"new_steps":new_steps,"source_views":len(manifest["source_files"]),"visual_review":["learning-index.desktop.png","slot-reuse.desktop.png","rewrite-send.desktop.png","install-overview.desktop.png","install-publish.desktop.png"],"limitations":["No DPDK runtime or packet delivery validation","Existing pre-Goal whitespace warnings were preserved","Generated learning material does not establish learner mastery","One static cleanup concern recorded for separate fault-injection investigation"]}
(OUT/"completion-audit.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
print(json.dumps({"complete":True,"requirements":len(requirements),"pages":9,"steps":total_steps,"source_files_compared":source["source_count"]}))

