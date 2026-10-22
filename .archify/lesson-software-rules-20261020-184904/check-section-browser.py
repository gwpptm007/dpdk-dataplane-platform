import base64, json, pathlib, shutil, socket, subprocess, tempfile, time, urllib.request
import websocket

root = pathlib.Path(r"E:\GitHub-Study\dpdk-dataplane-platform\.archify\lesson-software-rules-20261020-184904")
page = root / "software-rules-learning.html"
profile = pathlib.Path(tempfile.mkdtemp(prefix="dpdk-learning-chrome-")).resolve()
assert profile.is_relative_to(pathlib.Path(tempfile.gettempdir()).resolve())
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
proc = subprocess.Popen([
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
    "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=" + str(port),
    "--remote-allow-origins=*", "--user-data-dir=" + str(profile), page.as_uri()
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ws = None
receipt = {"page": str(page), "checks": [], "runtimeErrors": []}
try:
    targets = []
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen("http://127.0.0.1:%s/json/list" % port, timeout=1) as response:
                targets = json.load(response)
            if any(t.get("type") == "page" for t in targets):
                break
        except (OSError, ValueError):
            pass
        time.sleep(.15)
    target = next(t for t in targets if t.get("type") == "page")
    ws = websocket.create_connection(target["webSocketDebuggerUrl"], timeout=10, suppress_origin=True)
    seq = 0
    def cdp(method, params=None):
        global seq
        seq += 1
        ident = seq
        ws.send(json.dumps({"id": ident, "method": method, "params": params or {}}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get("method") == "Runtime.exceptionThrown":
                receipt["runtimeErrors"].append(msg["params"])
            if msg.get("id") == ident:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg.get("result", {})
    def js(expression):
        result = cdp("Runtime.evaluate", {"expression": expression, "returnByValue": True})
        if "exceptionDetails" in result:
            raise RuntimeError(result["exceptionDetails"])
        return result["result"].get("value")
    def check(name, condition):
        assert condition, name
        receipt["checks"].append(name)
    def click(selector):
        box = js("(()=>{const b=document.querySelector(" + json.dumps(selector) + ");b.scrollIntoView({block:'center'});const r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()")
        cdp("Input.dispatchMouseEvent", {"type": "mousePressed", "button": "left", "clickCount": 1, **box})
        cdp("Input.dispatchMouseEvent", {"type": "mouseReleased", "button": "left", "clickCount": 1, **box})
    cdp("Runtime.enable")
    cdp("Page.enable")
    cdp("Emulation.setDeviceMetricsOverride", {"width": 1440, "height": 1050, "deviceScaleFactor": 1, "mobile": False})


    def ready(expression):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            try:
                if js(expression):
                    return
            except (RuntimeError,KeyError):
                pass
            time.sleep(.05)
        raise AssertionError(expression)
    ready("document.querySelectorAll('[data-step]').length===6")
    check("rules page loads six steps", js("document.title.includes('1.3') && document.querySelector('[aria-current=step]').dataset.step==='0'"))
    for port in [9000,53]:
        click('[data-port="%s"]'%port)
        for index in range(6):
            click('[data-step="%s"]'%index)
            check("port %s step %s renders explanation and code"%(port,index+1),js("document.querySelector('[aria-current=step]').dataset.step==='"+str(index)+"' && document.getElementById('detail-title').textContent.startsWith('"+str(index+1)+"') && document.getElementById('detail-code').textContent.length>40 && !document.body.innerText.includes('undefined')"))
            if index==4:
                check("software drop result is correct for port "+str(port),js("document.getElementById('fields').textContent.includes('drop = "+("true" if port==9000 else "false")+"')"))
            if index==5:
                if port==9000:
                    check("matched rule produces policy DROP",js("document.getElementById('fields').textContent.includes('decision.action = DROP') && document.getElementById('fields').textContent.includes('POLICY')"))
                else:
                    check("unmatched rule defers egress instead of declaring FORWARD",js("document.getElementById('fields').textContent.includes('software_decision.drop = false') && !document.getElementById('fields').textContent.includes('FORWARD')"))
    click("#previous")
    check("previous step works",js("document.querySelector('[aria-current=step]').dataset.step==='4'"))
    click("#next")
    check("next step works",js("document.querySelector('[aria-current=step]').dataset.step==='5'"))
    click("header a")
    ready("location.pathname.endsWith('/packet-parse-learning.html') && document.querySelector('[aria-current=step]')?.dataset.step==='5'")
    check("backlink returns to parse return step",js("!document.getElementById('next-section').hidden"))
    click("#next-section")
    ready("location.pathname.endsWith('/software-rules-learning.html') && document.querySelector('[aria-current=step]')?.dataset.step==='0'")
    check("parse next-section link opens software rules",True)
    first=root.parent/"workflow-software-packet-path-20261020-143318"/"software-packet-learning.html"
    cdp("Page.navigate",{"url":first.as_uri()})
    ready("document.querySelector('nav[aria-label] a[href*=software-rules-learning]')!==null")
    click('nav a[href*="software-rules-learning"]')
    ready("location.pathname.endsWith('/software-rules-learning.html') && document.querySelectorAll('[data-step]').length===6")
    check("overview navigation opens software rules",True)
    check("desktop no horizontal overflow",js("document.documentElement.scrollWidth<=innerWidth"))
    js("window.scrollTo(0,0)")
    size=cdp("Page.getLayoutMetrics")["cssContentSize"]
    cap=cdp("Page.captureScreenshot",{"format":"png","captureBeyondViewport":True,"clip":{"x":0,"y":0,"width":size["width"],"height":size["height"],"scale":1}})
    image=root/"software-rules-learning.desktop.png"
    image.write_bytes(base64.b64decode(cap["data"]))
    cdp("Emulation.setDeviceMetricsOverride",{"width":800,"height":1000,"deviceScaleFactor":1,"mobile":False})
    click('[data-step="4"]')
    click('[data-port="53"]')
    check("narrow view updates software result",js("document.getElementById('fields').textContent.includes('drop = false')"))
    check("narrow no horizontal overflow",js("document.documentElement.scrollWidth<=innerWidth"))
    check("no runtime exceptions",not receipt["runtimeErrors"])
    receipt.update({"passed":True,"screenshot":str(image)})
    (root/"software-rules-learning.browser-check.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")
    print(json.dumps(receipt,ensure_ascii=True))
finally:
    if ws:
        try:
            ws.send(json.dumps({"id":9999,"method":"Browser.close"}))
            ws.close()
        except Exception:
            pass
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=5)
    assert profile.is_relative_to(pathlib.Path(tempfile.gettempdir()).resolve()) and profile.name.startswith("dpdk-learning-chrome-")
    shutil.rmtree(profile, ignore_errors=True)

