import pathlib
root = pathlib.Path(__file__).resolve().parent
for name in ["prepare-browser-check.py", "check-lesson-browser.py"]:
    p = root / name
    text = p.read_text(encoding="utf-8")
    text = text.replace("/software-packet-learning.html')", "/software-packet-learning.html#step-2')")
    p.write_text(text,encoding="utf-8")
base = (root/"check-lesson-browser.py").read_text(encoding="utf-8")
head = base[:base.index('    check("six teaching steps')]
tail = base[base.index("finally:"):]
body = r'''
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
'''
(root/"check-lesson-navigation.py").write_text(head+body+tail,encoding="utf-8")

