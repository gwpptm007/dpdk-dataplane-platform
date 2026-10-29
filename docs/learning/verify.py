import base64, datetime, hashlib, html, json, pathlib, re, shutil, socket, subprocess, tempfile, time, urllib.parse, urllib.request
from html.parser import HTMLParser
import websocket
from source_audit import ROOT, OUT
manifest=json.loads((OUT/"manifest.json").read_text(encoding="utf-8"))
chapter_data=json.loads((OUT/"chapters.json").read_text(encoding="utf-8"))
step_counts={"section-"+c["id"]+".html":len(c["steps"]) for c in chapter_data}
new_step_count=sum(step_counts.values())
pages=[(OUT/p).resolve() for p in manifest["pages"]]
report={"checks":[],"runtimeErrors":[],"screenshots":[],"time":datetime.datetime.now(datetime.timezone.utc).isoformat(),"pages":[]}
def check(name,condition):
    if not condition: raise AssertionError(name)
    report["checks"].append(name)
class Links(HTMLParser):
    def __init__(self):super().__init__();self.links=[];self.ids=set()
    def handle_starttag(self,tag,attrs):
        a=dict(attrs)
        if "id" in a:self.ids.add(a["id"])
        if tag=="a" and "href" in a:self.links.append(a["href"])
def review(passed=False):
    audit=json.loads((OUT/"source-check.json").read_text(encoding="utf-8"))
    status="浏览器、源码引用和本地链接检查已通过" if passed else "页面已生成，浏览器与链接检查进行中"
    changed="".join("<li>"+html.escape(f)+"</li>" for f in audit["changed_files"])
    observation=("静态阅读发现一个需单独核实的异常路径：control_apply_impl 在事务提交成功后，"
    "若更新旧版本的删除失败，会直接撤销新后端对象并返回；该分支未经过 transaction_finalize 或事务 rollback。"
    "软件 prepare 的 pending 通常由这两个入口释放，因此这里存在准备记录未释放的疑点。"
    "已定位当前源码中的更新失败分支，未执行故障注入，也未修改运行逻辑；不把它写成已验证的运行故障。")
    text='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>本阶段范围与验证</title>
<style>body{font:16px/1.8 "Microsoft YaHei",sans-serif;color:#183047;background:#f3f6fa;max-width:1000px;margin:auto;padding:24px}section{background:white;padding:22px;border:1px solid #dce5ec;border-radius:12px;margin:16px 0}a{color:#215ba4}code{overflow-wrap:anywhere}</style>
<a href="index.html">← 学习目录</a><h1>当前阶段的覆盖与验证</h1><section><h2>材料状态</h2><p>'''+status+'''</p><p>此前讲过的 1.1–1.3 保留原有形式并更新源码引用，新增 1.4–1.5 和 2.1–2.4 共 36 步。槽位删除与复用为本次补充，其余为此前学习内容的集中整理；材料完成不代表个人已经掌握。</p></section>
<section><h2>源码注释范围</h2><ul>'''+changed+'''</ul><p>以本 Goal 开始时工作区为基线，比较 lib、include、app/apps 中所有 C/H 的非注释词法内容；已有未提交修改保留。没有提交或 push，没有进行 DPDK 编译或真实流量测试。</p><a href="source-check.json">源码检查记录</a></section>
<section><h2>两条主线与边界</h2><p>报文：收包、解析、软件匹配、路由/ND、普通改写、TX与释放；规则：准备提交、匹配表、发布快照、账本索引、清理保存入口、删除回滚和QSBR。</p><p>运行时软件快照、路由快照、控制面账本与磁盘文件分别说明。IPv4三层改写仅总结MAC、TTL及头部校验和；IPv6按此前阅读展开。持久化格式及恢复、硬件规划/rte_flow、连接跟踪/NAT和完整启动接口列为后续待学。</p></section>
<section><h2>页面检查</h2><p>使用 file:// 在独立无头 Chrome 中逐节打开，检查步骤、前后按钮、连续阅读、原有 sent/端口选项、章节导航、源码行锚点和宽窄屏。静态源码节选按源文件行内容逐一比较。界面演示不执行DPDK。</p><a href="browser-check.json">浏览器与链接检查记录</a> · <a href="manifest.json">源码引用清单</a></section>
<section><h2>留待单独核实</h2><p>'''+observation+'''</p><a href="sources/lib__control__control.c.html">control.c 完整源码副本</a></section>
<section><h2>复现与继续</h2><p>源码注释或页面内容变动后，依次运行本目录 build.py、source_audit.py check、verify.py。重新开一个编辑任务时应另建基线，不能覆盖本次基线。</p><p>建议先从 2.3 第 6 步复习删除与复用，再沿目录将两条主线各读一遍；下一阶段选择配置持久化与启动恢复。</p></section></html>'''
    text=text.replace("共 36 步", "共 "+str(new_step_count)+" 步；2.1 已按客户端、服务端、事务安装和返回过程细分为 16 步")
    (OUT/"review.html").write_text(text,encoding="utf-8")
review()
(OUT/"browser-check.json").write_text(json.dumps({"passed":False,"status":"verification in progress"}),encoding="utf-8")
# Exact source spans and all static local hrefs, including generated source anchors
for i,r in enumerate(manifest["steps"]):
    src=(ROOT/r["file"]).read_text(encoding="utf-8").splitlines()
    check("source span "+str(i),"\n".join(src[r["line"]-1:r["endLine"]]).rstrip()==r["code"])
    assert r["fn"] in (ROOT/r["file"]).read_text(encoding="utf-8")
inspect=pages+[OUT/"index.html",OUT/"review.html"]+list((OUT/"sources").glob("*.html"))
parsers={}
for p in inspect:
    parser=Links();parser.feed(p.read_text(encoding="utf-8"));parsers[p.resolve()]=parser
count_links=0
for p in inspect:
    for href in parsers[p.resolve()].links:
        parts=urllib.parse.urlsplit(href)
        if parts.scheme or href.startswith("//"):continue
        dest=(p.parent/urllib.parse.unquote(parts.path)).resolve() if parts.path else p.resolve()
        check("local href "+str(count_links)+" "+str(dest),dest.is_file());count_links+=1
        if parts.fragment:
            match=re.fullmatch(r"step-(\d+)",parts.fragment)
            if match and dest in pages:
                expected=step_counts.get(dest.name,5 if dest.name=="software-packet-learning.html" else 6)
                check("step anchor "+str(count_links),1<=int(match[1])<=expected)
                continue
            if dest in parsers:check("anchor "+str(count_links),urllib.parse.unquote(parts.fragment) in parsers[dest].ids)
report["local_links"]=count_links
profile=pathlib.Path(tempfile.mkdtemp(prefix="dpdk-stage-review-")).resolve()
with socket.socket() as sock:sock.bind(("127.0.0.1",0));port=sock.getsockname()[1]
proc=subprocess.Popen([r"C:\Program Files\Google\Chrome\Application\chrome.exe","--headless=new","--disable-gpu","--no-first-run","--no-default-browser-check","--remote-debugging-address=127.0.0.1","--remote-debugging-port="+str(port),"--remote-allow-origins=*","--user-data-dir="+str(profile),"about:blank"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
ws=None;seq=0
try:
    deadline=time.monotonic()+25
    while True:
        try:
            with urllib.request.urlopen("http://127.0.0.1:%s/json/list"%port,timeout=1) as r:targets=json.load(r)
            target=next(t for t in targets if t.get("type")=="page");break
        except (OSError,ValueError,StopIteration):
            if time.monotonic()>deadline:raise
            time.sleep(.1)
    ws=websocket.create_connection(target["webSocketDebuggerUrl"],timeout=15,suppress_origin=True)
    def cdp(method,params=None):
        global seq
        seq+=1;ident=seq
        ws.send(json.dumps({"id":ident,"method":method,"params":params or {}}))
        while True:
            msg=json.loads(ws.recv())
            if msg.get("method")=="Runtime.exceptionThrown":report["runtimeErrors"].append(msg["params"])
            if msg.get("id")==ident:
                if "error" in msg:raise RuntimeError(msg["error"])
                return msg.get("result",{})
    def js(exp):
        r=cdp("Runtime.evaluate",{"expression":exp,"returnByValue":True})
        if "exceptionDetails" in r:raise RuntimeError(r["exceptionDetails"])
        return r["result"].get("value")
    def viewport(width):
        cdp("Emulation.setDeviceMetricsOverride",{"width":width,"height":1100,"deviceScaleFactor":1,"mobile":False})
    def openpage(p,fragment=""):
        url=p.as_uri()+fragment
        cdp("Page.navigate",{"url":url})
        until=time.monotonic()+8
        while time.monotonic()<until:
            if js("location.href==="+json.dumps(url)+" && document.readyState==='complete'"):return
            time.sleep(.05)
        raise AssertionError("navigation timeout "+url)
    def click(selector):
        box=js("(()=>{const b=document.querySelector("+json.dumps(selector)+");b.scrollIntoView({block:'center'});const r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()")
        for typ in ("mousePressed","mouseReleased"):cdp("Input.dispatchMouseEvent",{"type":typ,"button":"left","clickCount":1,**box})
    def shot(name,top=True):
        if top: js("scrollTo(0,0)")
        data=cdp("Page.captureScreenshot",{"format":"png"})
        (OUT/name).write_bytes(base64.b64decode(data["data"]));report["screenshots"].append(name)
    cdp("Runtime.enable");cdp("Page.enable")
    for p in pages:
        viewport(1440);openpage(p)
        name=p.name
        check(name+" utf8",js("!document.body.innerText.includes('\\ufffd')"))
        check(name+" shared chapter navigation",js("document.querySelectorAll('#stage-nav a').length===10"))
        expected=step_counts.get(name,5 if name=="software-packet-learning.html" else 6)
        check(name+" step count",js("document.querySelectorAll('[data-step]').length")==expected)
        for i in range(expected):
            if name=="section-2.1.html":
                phase=js("steps["+str(i)+"].phase")
                click('[data-phase="%d"]'%phase)
            click('[data-step="%d"]'%i)
            check(name+" step "+str(i),js("document.querySelector('[aria-current=step]').dataset.step==="+json.dumps(str(i))+" && document.getElementById('detail-code').textContent.trim().length>10 && document.getElementById('detail-input').textContent.length>0 && document.getElementById('detail-output').textContent.length>0"))
        click("#previous");check(name+" previous",js("document.querySelector('[aria-current=step]').dataset.step")==str(expected-2))
        click("#next");check(name+" next",js("document.querySelector('[aria-current=step]').dataset.step")==str(expected-1))
        if name.startswith("section-"):
            click("#show-all");check(name+" continuous reading",js("!document.getElementById('all-steps').hidden && document.querySelectorAll('.full-step').length==="+str(expected)))
            click("#show-all")
            check(name+" visible state",js("document.getElementById('fields').textContent.length>10"))
        if name=="section-2.1.html":
            check("install full path and phases",js("document.querySelectorAll('[data-phase]').length===4 && steps.length===16"))
            check("install last step links worker active",js("document.getElementById('detail-code').textContent.includes('atomic_load_explicit') && document.getElementById('pending-value').textContent.includes('释放')"))
            click('[data-phase="0"]')
            check("install phase returns to first step",js("document.querySelector('[aria-current=step]').dataset.step==='0'"))
            check("install initial snapshot is A",js("document.getElementById('snapshot-value').textContent.includes('A')"))
            initial_code=js("document.getElementById('detail-code').textContent")
            click('[data-code="1"]')
            check("install source tabs work",js("document.getElementById('detail-code').textContent")!=initial_code)
            click('[data-code="0"]')
            check("install source tab returns",js("document.getElementById('detail-code').textContent")==initial_code)
            shot("install-overview.desktop.png")
            click('[data-phase="2"]');click('[data-step="11"]')
            check("install published state is B",js("document.getElementById('snapshot-value').textContent.includes('active → B') && document.getElementById('detail-code').textContent.includes('atomic_exchange_explicit')"))
            shot("install-publish.desktop.png")
            js("scrollTo(0,document.querySelector('.detail').getBoundingClientRect().top+scrollY-105)")
            check("install state stays visible while reading code",js("document.querySelector('.state-board').getBoundingClientRect().top>=0 && document.querySelector('.state-board').getBoundingClientRect().bottom<180"))
            shot("install-reader.desktop.png",False)
        if name=="software-packet-learning.html":
            for sent in range(3):
                click('[data-sent="%d"]'%sent)
                check("TX ownership "+str(sent),js("[...document.querySelectorAll('#driver-packets .packet')].map(e=>e.textContent).join('')")==["","B","BC"][sent])
                check("TX worker tail "+str(sent),js("[...document.querySelectorAll('#worker-packets .packet')].map(e=>e.textContent).join('')")==["BC","C",""][sent])
        elif name in ("packet-parse-learning.html","software-rules-learning.html"):
            for port_value in (53,9000):
                click('[data-port="%d"]'%port_value)
                check(name+" input port "+str(port_value),js("document.getElementById('input-port').textContent")==str(port_value))
            if name=="software-rules-learning.html":
                click('[data-port="53"]');check("rule miss branch",js("document.getElementById('detail-output').textContent.includes('出口')"))
                click('[data-port="9000"]');check("rule hit branch",js("document.getElementById('detail-output').textContent.includes('DROP')"))
        for width in (1440,800,390):
            viewport(width);check(name+" no overflow "+str(width),js("document.documentElement.scrollWidth<=innerWidth"))
        viewport(1440)
        openpage(p,"#step-"+str(expected))
        check(name+" deep link",js("document.querySelector('[aria-current=step]').dataset.step")==str(expected-1))
        if name=="section-2.3.html":shot("slot-reuse.desktop.png")
        if name=="section-1.5.html":
            click('[data-step="4"]');shot("rewrite-send.desktop.png")
        # Actually navigate a source link and verify its real DOM line target
        selector="#source-links a" if name.startswith("section-") else "#review-sources a"
        href=js("document.querySelector("+json.dumps(selector)+").href")
        cdp("Page.navigate",{"url":href})
        until=time.monotonic()+8
        while time.monotonic()<until:
            if js("document.readyState==='complete' && location.href==="+json.dumps(href)):break
            time.sleep(.05)
        check(name+" source target",js("!!document.querySelector(':target')"))
        report["pages"].append({"page":name,"steps":expected})
    openpage(OUT/"index.html");viewport(1440);shot("learning-index.desktop.png")
    click('a[href="section-2.3.html#step-6"]')
    until=time.monotonic()+8
    while time.monotonic()<until:
        if js("location.pathname.endsWith('section-2.3.html') && document.readyState==='complete'"):break
        time.sleep(.05)
    check("directory continue link",js("document.querySelector('[aria-current=step]').dataset.step==='5'"))
    # Validate each new chapter's previous/next links by real browser navigation
    for p in pages[3:]:
        openpage(p)
        href=js("document.querySelector('.section-pager a:last-child').href")
        click(".section-pager a:last-child")
        until=time.monotonic()+8
        while time.monotonic()<until:
            if js("location.href==="+json.dumps(href)+" && document.readyState==='complete'"):break
            time.sleep(.05)
        check(p.name+" chapter next",js("location.href==="+json.dumps(href)))
    check("no JavaScript exceptions",not report["runtimeErrors"])
    report["passed"]=True
    review(True)
    report["artifact_hashes"]={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inspect}
    (OUT/"browser-check.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps({"passed":True,"pages":len(report["pages"]),"checks":len(report["checks"]),"local_links":count_links,"screenshots":report["screenshots"]},ensure_ascii=False))
finally:
    if ws:
        try:ws.send(json.dumps({"id":99999,"method":"Browser.close"}));ws.close()
        except Exception:pass
    try:proc.wait(timeout=8)
    except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=5)
    assert profile.is_relative_to(pathlib.Path(tempfile.gettempdir()).resolve()) and profile.name.startswith("dpdk-stage-review-")
    shutil.rmtree(profile,ignore_errors=True)

