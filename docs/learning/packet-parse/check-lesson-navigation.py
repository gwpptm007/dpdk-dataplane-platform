import base64, json, pathlib, shutil, socket, subprocess, tempfile, time, urllib.request
import websocket

root = pathlib.Path(__file__).resolve().parent
page = root / "packet-parse-learning.html"
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
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            try:
                if js(expression):
                    return
            except (RuntimeError, KeyError):
                pass
            time.sleep(.05)
        raise AssertionError(expression)
    previous = root.parent / "packet-path" / "software-packet-learning.html"
    cdp("Page.navigate",{"url":previous.as_uri()+"#step-2"})
    ready("document.querySelector('[aria-current=step]')?.dataset.step==='1'")
    check("deep link opens parent step 2", js("!document.getElementById('parse-lesson-entry').hidden"))
    click("#parse-lesson-entry a")
    ready("location.pathname.endsWith('/packet-parse-learning.html') && document.querySelectorAll('[data-step]').length===6")
    check("parent step opens parsing lesson", js("document.querySelector('[aria-current=step]').dataset.step==='0'"))
    click("header a")
    ready("location.hash==='#step-2' && document.getElementById('parse-lesson-entry')!==null")
    check("return restores parent step 2", js("document.querySelector('[aria-current=step]').dataset.step==='1' && !document.getElementById('parse-lesson-entry').hidden"))
    click('[data-step="4"]')
    check("lesson end offers next lesson", js("!document.getElementById('lesson-end').hidden && !document.getElementById('next-lesson').hidden && document.getElementById('next').hidden"))
    click("#next-lesson")
    ready("location.pathname.endsWith('/packet-parse-learning.html') && document.querySelectorAll('[data-step]').length===6")
    check("lesson end opens parsing lesson", js("document.querySelector('[aria-current=step]').dataset.step==='0'"))
    click(".lesson-route a")
    ready("location.hash==='#step-2' && document.getElementById('parse-lesson-entry')!==null")
    check("course breadcrumb returns to parent", js("document.querySelector('[aria-current=step]').dataset.step==='1'"))
    click('header a[href*="packet-parse-learning"]')
    ready("location.pathname.endsWith('/packet-parse-learning.html') && document.querySelectorAll('[data-step]').length===6")
    check("parent header opens next lesson", True)
    click("header a")
    ready("location.hash==='#step-2' && document.getElementById('parse-lesson-entry')!==null")
    js("window.scrollTo(0,0)")
    check("no horizontal overflow", js("document.documentElement.scrollWidth<=innerWidth"))
    size=cdp("Page.getLayoutMetrics")["cssContentSize"]
    capture=cdp("Page.captureScreenshot",{"format":"png","captureBeyondViewport":True,"clip":{"x":0,"y":0,"width":size["width"],"height":size["height"],"scale":1}})
    screenshot=root/"lesson-navigation.desktop.png"
    screenshot.write_bytes(base64.b64decode(capture["data"]))
    check("no runtime exceptions",not receipt["runtimeErrors"])
    receipt.update({"passed":True,"screenshot":str(screenshot)})
    (root/"lesson-navigation.browser-check.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")
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

