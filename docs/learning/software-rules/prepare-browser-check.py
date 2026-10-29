import pathlib
root=pathlib.Path(__file__).resolve().parent
previous=root.parent/"packet-parse"/"check-lesson-browser.py"
base=previous.read_text(encoding="utf-8")
head=base[:base.index('    check("six teaching steps')]
head=head.replace('page = root / "packet-parse-learning.html"', 'page = root / "software-rules-learning.html"')
tail=base[base.index("finally:"):]
body=r'''
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
    first=root.parent/"packet-path"/"software-packet-learning.html"
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
'''
(root/"check-section-browser.py").write_text(head+body+tail,encoding="utf-8")

