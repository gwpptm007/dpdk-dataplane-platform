import base64, json, pathlib, shutil, socket, subprocess, tempfile, time, urllib.request
import websocket

root = pathlib.Path(r"E:\GitHub-Study\dpdk-dataplane-platform\.archify\workflow-software-packet-path-20261020-143318")
page = root / "software-packet-learning.html"
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
    check("page loaded and UTF-8 intact", js("document.title.includes('DPDK') && !document.body.innerText.includes('\\ufffd') && document.querySelectorAll('[data-step]').length===5"))
    check("sent visible without clicking", js("document.getElementById('sent-value').getBoundingClientRect().bottom < innerHeight && document.getElementById('sent-value').textContent==='1'"))
    for index in range(5):
        click('[data-step="%s"]' % index)
        check("step %s updates explanation and code" % (index+1), js("document.querySelector('[aria-current=step]').dataset.step==='"+str(index)+"' && document.getElementById('detail-title').textContent.startsWith('"+str(index+1)+"') && document.getElementById('detail-code').textContent.length>40 && document.getElementById('detail-explain').textContent.length>15"))
    for sent in range(3):
        click('[data-sent="%s"]' % sent)
        driver = ["", "B", "BC"][sent]
        worker = ["BC", "C", ""][sent]
        actual = js("({driver:[...document.querySelectorAll('#driver-packets .packet')].map(e=>e.textContent).join(''),worker:[...document.querySelectorAll('#worker-packets .packet')].map(e=>e.textContent).join(''),sent:document.getElementById('sent-value').textContent})")
        check("sent %s produces correct ownership" % sent, actual == {"driver":driver,"worker":worker,"sent":str(sent)})
    click('[data-sent="1"]')
    click('[data-step="3"]')
    click('#next')
    check("next navigation works", js("document.querySelector('[aria-current=step]').dataset.step==='4'"))
    click('#previous')
    check("previous navigation works", js("document.querySelector('[aria-current=step]').dataset.step==='3'"))
    check("desktop has no horizontal overflow", js("document.documentElement.scrollWidth<=innerWidth"))
    js("window.scrollTo(0,0)")
    cdp("Page.bringToFront")
    metrics = cdp("Page.getLayoutMetrics")
    size = metrics["cssContentSize"]
    capture = cdp("Page.captureScreenshot", {"format":"png","captureBeyondViewport":True,"clip":{"x":0,"y":0,"width":size["width"],"height":size["height"],"scale":1}})
    image = root / "software-packet-learning.desktop.png"
    image.write_bytes(base64.b64decode(capture["data"]))
    receipt["screenshot"]=str(image)
    receipt["viewport"]={"width":1440,"height":1050}
    cdp("Emulation.setDeviceMetricsOverride", {"width":800,"height":1000,"deviceScaleFactor":1,"mobile":False})
    check("narrow viewport has no horizontal overflow", js("document.documentElement.scrollWidth<=innerWidth"))
    click('[data-step="1"]')
    check("step click works at narrow viewport", js("document.querySelector('[aria-current=step]').dataset.step==='1'"))
    check("no runtime exceptions", not receipt["runtimeErrors"])
    receipt["passed"]=True
    (root/"software-packet-learning.browser-check.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")
    print(json.dumps(receipt, ensure_ascii=True))
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

