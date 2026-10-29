import html, json, pathlib, re, subprocess, hashlib, os, sys
from source_audit import ROOT, OUT
H=html.escape
LEGACY=[
 ("1.1","收发与归属",ROOT/"docs/learning/packet-path/software-packet-learning.html"),
 ("1.2","报文解析",ROOT/"docs/learning/packet-parse/packet-parse-learning.html"),
 ("1.3","软件规则",ROOT/"docs/learning/software-rules/software-rules-learning.html")]
chapters=json.loads((OUT/"chapters.json").read_text(encoding="utf-8"))
progress=json.loads((OUT/"progress.json").read_text(encoding="utf-8"))
files=set(); evidence=[]
def fname(file): return file.replace("/","__")+".html"
def rel(path,base): return os.path.relpath(path,base).replace("\\","/")
def function(file,fn):
    text=(ROOT/file).read_text(encoding="utf-8")
    m=re.search(r"(?m)^[A-Za-z_][^\n;{}]*\b"+re.escape(fn)+r"\s*\([^;{}]*\)\s*\{",text)
    if not m: raise ValueError((file,fn,"definition missing"))
    end=text.find("\n}",m.start())+2
    assert end>m.end(),(file,fn)
    return text,m.start(),end
def ref(file,fn,anchor=None,n=None):
    text,start,end=function(file,fn)
    if anchor is not None and anchor not in text[start:end]: raise ValueError((file,fn,anchor,"anchor missing"))
    pos=start if anchor is None else text.index(anchor,start,end)
    start=text.rfind("\n",0,pos)+1
    if n: end=min(end,start+sum(len(x) for x in text[start:].splitlines(keepends=True)[:n]))
    line=text.count("\n",0,start)+1
    code=text[start:end].rstrip()
    # Never end a displayed excerpt in the next explanatory comment
    if code.count('/*')>code.count('*/'): code=code[:code.rfind('/*')].rstrip()
    while re.search(r'/\*[^*]*(?:\*(?!/)[^*]*)*\*/\s*$',code):
        code=re.sub(r'/\*[^*]*(?:\*(?!/)[^*]*)*\*/\s*$','',code).rstrip()
    files.add(file)
    r={"file":file,"fn":fn,"line":line,"endLine":line+len(code.splitlines())-1,
       "code":code,"sourceHref":"sources/"+fname(file)+"#L"+str(line)}
    return r
def step(s):
    r=ref(s["file"],s["fn"],s.get("anchor"),s.get("n"))
    related=[ref(*item) for item in s.get('related',[])]
    result={**s,**r,'related':related}; evidence.append(r); evidence.extend(related)
    return result
allpages=LEGACY+[(c["id"],c["title"],OUT/("section-"+c["id"]+".html")) for c in chapters]
def nav(base,current):
    links=['<a href="'+H(rel(OUT/"index.html",base))+'">学习目录</a>']
    for id,title,path in allpages:
        links.append('<a '+('aria-current="page" ' if current==id else '')+'href="'+H(rel(path,base))+'">'+H(id+" "+title)+'</a>')
    return '<nav id="stage-nav" class="stage-nav" aria-label="完整章节导航">'+" · ".join(links)+'</nav>'
def insert_nav(text,base,current):
    text=re.sub(r'<nav id="stage-nav"[\s\S]*?</nav>\s*',"",text)
    return text.replace("</header>","</header>\n"+nav(base,current),1)
style=re.search(r"<style>([\s\S]*?)</style>",LEGACY[0][2].read_text(encoding="utf-8")).group(1)
extra=".stage-nav{display:flex;flex-wrap:wrap;gap:5px 12px;margin:14px 0;font-size:13px}.stage-nav a[aria-current=page]{font-weight:bold}.source-review{font-size:13px;margin:7px 0;overflow-wrap:anywhere}"
# Match legacy descriptions to real definitions; avoid manually maintaining line numbers
W="lib/dataplane/worker.c"; P="lib/dataplane/pipeline.c"; M="lib/dataplane/packet_mbuf.c"; PK="lib/dataplane/packet.c"; S="lib/dataplane/software_backend.c"
legacy_refs=[
 [[(W,"process_ingress")],[(W,"process_batch"),(M,"dppd_packet_parse_mbuf"),(W,"routed_decide"),(P,"dppd_pipeline_decide_counted")],[(W,"process_batch")],[(W,"flush_egress","uint16_t sent =",1)],[(W,"flush_egress","free_packets(packets + sent",1)]],
 [[(W,"process_batch","int parsed =",1),(M,"dppd_packet_parse_mbuf")],[(M,"dppd_packet_parse_mbuf"),(PK,"dppd_packet_parse_buffer"),(PK,"dppd_packet_parse_headers")],[(PK,"dppd_packet_parse_headers","ether_type = read_be16",1)],[(PK,"dppd_packet_parse_headers","if (ether_type != DPPD_ETHER_TYPE_IPV4)",1)],[(PK,"dppd_packet_parse_headers","packet->l4_type = DPPD_L4_UDP",1)],[(PK,"dppd_packet_parse_buffer"),(M,"dppd_packet_parse_mbuf"),(W,"process_batch","int parsed =",1),(W,"routed_decide")]],
 [[(W,"process_batch","routed_decide(runtime, ingress",1),(W,"routed_decide"),(P,"dppd_pipeline_decide_counted")],[(P,"dppd_pipeline_decide_counted","memset(decision",1)],[(S,"dppd_software_backend_decide_counted"),("lib/dataplane/acl.c","dppd_acl_classify"),("lib/dataplane/exact.c","dppd_exact_classify")],[(S,"dppd_software_backend_decide_counted","if (exact_tag != 0",5),(S,"rule_before")],[(S,"dppd_software_backend_decide_counted","if (selected != NULL)",24),(S,"count_packet"),(W,"process_batch","dppd_software_count_batch_flush",1)],[(P,"dppd_pipeline_decide_counted","if (software_decision.drop)",6),(W,"process_batch","if (decision.action != DPPD_PACKET_FORWARD)",1)]]
]
def legacy_script(refs,page):
    data=[]
    for row in refs:
        data.append([{"href":rel(OUT/r["sourceHref"].split("#")[0],page.parent)+"#L"+str(r["line"]),"label":r["file"]+" · "+r["fn"]+" · L"+str(r["line"])} for r in row])
    return """<script id="review-source-script">
(()=>{const refs=DATA;const loc=document.getElementById('detail-location');const box=document.createElement('div');box.className='source-review';box.id='review-sources';loc.after(box);
function sync(){const b=document.querySelector('[data-step][aria-current=step]');const i=b?Number(b.dataset.step):0;box.replaceChildren();for(const r of refs[i]){const a=document.createElement('a');a.href=r.href;a.textContent=r.label;a.target='_blank';a.rel='noopener';box.append(a,document.createElement('br'));}}
new MutationObserver(sync).observe(loc,{childList:true,characterData:true,subtree:true});sync();})();
</script>""".replace("DATA",json.dumps(data,ensure_ascii=False))
for k,(id,title,page) in enumerate(LEGACY):
    refs=[[ref(*item) for item in row] for row in legacy_refs[k]]
    evidence.extend(r for row in refs for r in row)
    if k:
        name="lesson" if k==1 else "section"
        datafile=page.parent/(name+".json")
        raw=datafile.read_text(encoding="utf-8")
        try: items=json.loads(raw)
        except json.JSONDecodeError:
            # Repair the existing literal newline inside one location string only
            raw=raw.replace("·\n dppd_packet_parse_headers","· dppd_packet_parse_headers")
            items=json.loads(raw)
        for item,row in zip(items,refs):
            item["location"]=" → ".join(r["file"]+":"+str(r["line"])+" · "+r["fn"] for r in row)
        datafile.write_text(json.dumps(items,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
        templatefile=page.parent/(name+"-template.html")
    else: templatefile=page
    text=templatefile.read_text(encoding="utf-8")
    text=insert_nav(text,page.parent,id)
    text=text.replace("｜后续：1.4 出口选择",'｜<a href="'+rel(OUT/"section-1.4.html",page.parent)+'">继续 1.4 出口选择与 ND</a>')
    text=re.sub(r'<script id="review-source-script">[\s\S]*?</script>',"",text)
    if "/* stage-review-style */" not in text:
        text=text.replace("</style>","/* stage-review-style */\n"+extra+"\n</style>",1)
    text=text.replace("</body>",legacy_script(refs,page)+"</body>")
    if k==0:
        # Replace all five location literals while preserving user-authored explanations and sent controls
        matches=list(re.finditer(r"location:'[^']*'",text))
        if not matches: matches=list(re.finditer(r'location:"[^"\n]*"',text))
        assert len(matches)==5
        for m,row in reversed(list(zip(matches,refs))):
            text=text[:m.start()]+"location:"+json.dumps(" → ".join(r["file"]+":"+str(r["line"])+" · "+r["fn"] for r in row),ensure_ascii=False)+text[m.end():]
        text=text.replace("return match ? Number(match[1]) - 1 : 3;","return match ? Number(match[1]) - 1 : 0;")
    if k==2 and 'id="review-next-section"' not in text:
        link='<p><a id="review-next-section" href="'+rel(OUT/"section-1.4.html",page.parent)+'">下一节 · 1.4 出口选择与 ND →</a></p>'
        text=text.replace("</article>",link+"</article>")
    templatefile.write_text(text,encoding="utf-8")
    if k: subprocess.run([sys.executable,str(page.parent/("build-"+name+".py"))],check=True,cwd=ROOT)
template=(OUT/"chapter-template.html").read_text(encoding="utf-8")
for i,c in enumerate(chapters):
    c["steps"]=[step(s) for s in c["steps"]]
    idx=i+3
    prev=allpages[idx-1]; nxt=allpages[idx+1] if idx+1<len(allpages) else None
    replacements={
    "TITLE":H(c["id"]+" "+c["title"]),"CHAPTER":"第 "+c["id"][0]+" 章 · "+("报文处理主线" if c["id"].startswith("1") else "规则管理主线"),
    "STYLE":style,"NAV":nav(OUT,c["id"]),"STATUS":H(progress["sections"][c["id"]]["status"]),"SCOPE":H(c["scope"]),
    "EXAMPLE":H(c["example"]),"FLOW":H(c["flow"]),"STEPS":json.dumps(c["steps"],ensure_ascii=False).replace("</",r"<\/"),
    "PREVIOUS":'<a href="'+H(rel(prev[2],OUT))+'">← '+H(prev[0]+" "+prev[1])+'</a>',
    "NEXT":'<a href="'+H(rel(nxt[2],OUT))+'">'+H(nxt[0]+" "+nxt[1])+' →</a>' if nxt else '<a href="index.html">返回目录与后续安排 →</a>'}
    text=(OUT/"install-template.html").read_text(encoding="utf-8") if c["id"]=="2.1" else template
    for key,value in replacements.items(): text=text.replace("__"+key+"__",value)
    (OUT/("section-"+c["id"]+".html")).write_text(text.rstrip()+"\n",encoding="utf-8")
# Resolve progress links from the current source instead of storing line numbers.
current_ref=ref(progress["current"]["file"],progress["current"]["fn"])
next_ref=ref(progress["next"]["file"],progress["next"]["fn"])
caller_ref=ref(progress["next"]["file"],progress["next"]["caller_fn"],progress["next"]["caller_anchor"],1)
evidence.extend([current_ref,next_ref,caller_ref])
# Source copies allow actual line anchors to work in an ordinary local browser
(OUT/"sources").mkdir(exist_ok=True)
for file in sorted(files):
    text=(ROOT/file).read_text(encoding="utf-8")
    lines="".join('<div class="line" id="L'+str(i)+'"><a href="#L'+str(i)+'">'+str(i)+'</a><code>'+H(l)+'</code></div>' for i,l in enumerate(text.splitlines(),1))
    body='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+H(file)+'</title><style>body{font:14px/1.7 Consolas,"Microsoft YaHei",monospace;margin:20px;color:#183047;background:#f7f9fc}.line{display:flex;gap:16px;scroll-margin-top:20px}.line a{min-width:4em;color:#526579;text-align:right}.line code{white-space:pre-wrap;overflow-wrap:anywhere}.line:target{background:#ffe3a4}header{position:sticky;top:0;padding:12px;background:#eef4fb}a{color:#215ba4}</style><header><a href="../index.html">学习目录</a> · '+H(file)+' · 当前源码副本，修改源码后需重新构建</header>'+lines+'</html>'
    (OUT/"sources"/fname(file)).write_text(body,encoding="utf-8")
# Directory keeps both complete chains visible without selecting a step
cards=""
for id,title,path in allpages:
    status=progress["sections"][id]
    cards+='<a class="card entry" href="'+H(rel(path,OUT))+'"><h2>'+H(id+" "+title)+'</h2><p class="study-status">'+H(status["status"])+'</p><p>'+H(status["detail"])+'</p></a>'
material=progress.get("material_sync")
material_links=('''<details id="material-sync"><summary>最近补齐的讲解 · '''+H(material["updated_at"])+'''</summary><p>'''+H(material["description"])+'''</p><ul>'''+''.join('<li><a href="'+H(t["href"])+'">'+H(t["title"])+'</a></li>' for t in material["topics"])+'''</ul></details>''') if material else ''
progress_panel='''<section class="card learning-progress" id="learning-progress" aria-labelledby="progress-title">
<div class="eyebrow">学习进度 · 更新于 <time id="progress-date" datetime="'''+H(progress["updated_at"])+'''">'''+H(progress["updated_at"])+'''</time></div>
<h2 id="progress-title">当前停在：'''+H(progress["current"]["title"])+'''</h2>
<p>'''+H(progress["summary"])+'''</p>
<div class="progress-grid"><div><h3>刚读到哪里</h3><p>'''+H(progress["current"]["description"])+'''</p>
<p><a href="'''+H(progress["current"]["review_href"])+'''">回顾 2.3 的规则记录</a> · <a href="'''+H(current_ref["sourceHref"])+'''">'''+H(current_ref["fn"])+''' 源码</a></p></div>
<div class="next-reading"><h3>下一步：'''+H(progress["next"]["title"])+'''</h3><p>'''+H(progress["next"]["description"])+'''</p>
<a class="resume-link" id="resume-learning" href="'''+H(progress["next"]["href"])+'''">继续学习 · 2.4 配置保存 →</a>
<p class="progress-sources"><a href="'''+H(caller_ref["sourceHref"])+'''">control.c 的调用位置</a> · <a href="'''+H(next_ref["sourceHref"])+'''">'''+H(next_ref["fn"])+''' 源码</a></p></div></div>
<p class="progress-note">'''+H(progress["next"]["note"])+'''</p><details><summary>最近这轮走读的调用链</summary><pre>'''+H(progress["recent_flow"])+'''</pre></details>
'''+material_links+'''<p class="progress-note">'''+H(progress["basis"])+'''；页面点击只切换内容，进度由学习记录维护</p></section>'''
follow_up=''.join('<li><a href="'+H(s["href"])+'">'+H(s["title"])+'</a>：'+H(s["detail"])+'</li>' for s in progress["follow_up"])
later='；'.join(H(s) for s in progress["later"])
index='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DPDK 源码学习目录</title><style>'''+style+'''
.entries{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;margin:20px 0}.entry{text-decoration:none}.entry p{font-size:13px;color:var(--muted)}.entry .study-status{font-weight:700;color:var(--blue)}.flows{display:grid;grid-template-columns:1fr 1fr;gap:14px}.learning-progress{margin:20px 0;border-top:4px solid var(--blue)}.learning-progress h2{font-size:22px}.learning-progress h3{font-size:16px;margin:0 0 10px}.progress-grid{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:24px;margin:20px 0}.next-reading{padding:18px;background:#eef4fb;border-radius:8px}.resume-link{display:inline-block;background:var(--blue);color:white;padding:10px 16px;border-radius:6px;text-decoration:none;font-weight:700}.progress-note,.progress-sources{font-size:13px;color:var(--muted)}.progress-sources a{overflow-wrap:anywhere}.learning-progress summary{cursor:pointer;color:var(--blue)}.learning-progress pre{white-space:pre-wrap;overflow-wrap:anywhere}.follow-up li{margin:10px 0}@media(max-width:800px){.entries,.flows,.progress-grid{grid-template-columns:1fr}main{padding:18px}} </style><main>
<header><div><div class="eyebrow">源码学习 · 当前阶段整理</div><h1>一个包怎样处理，一条规则怎样生效</h1><p class="sub">从当前源码出发，按调用关系阅读；材料已整理与个人已经掌握是两件事</p></div></header>
'''+progress_panel+'''
<section class="flows"><div class="card"><h2>第 1 章 · 报文处理</h2><pre>收包 → 逐包解析 → 软件规则匹配
  → 路由 / 端口配对 → 静态邻居 / ND
  → 改写 → 按出口收集 → TX / 释放</pre></div>
<div class="card"><h2>第 2 章 · 规则管理</h2><pre>控制请求 → prepare / commit → exact / ACL
  → 发布 active → service->rules → finalize → 保存入口
删除 / 回滚 → 新快照 → QSBR 回收旧表</pre></div></section>
<div class="clear">阅读方法：先看每节顶部固定示例与完整路径，再读当前代码、输入输出和状态变化。可用“展开本节全部步骤”连续阅读。源码链接打开带行号的本地副本，返回目录可跨节复习。</div>
<h2>各节学习进度</h2><section class="entries">'''+cards+'''</section>
<section class="card follow-up"><h2>接下来按这条线继续</h2><ol>'''+follow_up+'''</ol><p>以上内容串起来后，再选择深入：'''+later+'''。</p><p class="progress-note">读代码时补充过的基础知识：'''+H('、'.join(progress["language_topics"]))+'''</p></section>
<footer><a href="review.html">查看本次范围、源码验证、浏览器检查及待核实问题</a> · 本地教学页面，不运行 DPDK、不发送报文</footer></main></html>'''
(OUT/"index.html").write_text(index,encoding="utf-8")
manifest={"pages":[rel(p,OUT) for _,_,p in allpages],"steps":evidence,"source_files":sorted(files)}
(OUT/"manifest.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding="utf-8")
print("Built",len(allpages),"chapters;",sum(len(c["steps"]) for c in chapters),"new steps;",len(files),"source views")

