import pathlib
root = pathlib.Path(r"E:\GitHub-Study\dpdk-dataplane-platform\.archify\lesson-packet-parse-20261020-161845")
old = root.parent / "workflow-software-packet-path-20261020-143318" / "check-learning-browser.py"
script = old.read_text(encoding="utf-8").replace('root = pathlib.Path(r"E:\\GitHub-Study\\dpdk-dataplane-platform\\.archify\\workflow-software-packet-path-20261020-143318")', 'root = pathlib.Path(r"E:\\GitHub-Study\\dpdk-dataplane-platform\\.archify\\lesson-packet-parse-20261020-161845")').replace("software-packet-learning", "packet-parse-learning")
start = script.index('    check("page loaded')
end = script.index('    check("desktop has no horizontal overflow"', start)
checks = r'''
    check("six teaching steps loaded", js("document.title.includes('DPDK') && document.querySelectorAll('[data-step]').length===6"))
    check("initial step is first", js("document.querySelector('[aria-current=step]').dataset.step==='0' && document.getElementById('input-port').textContent==='9000'"))
    for index in range(6):
        click('[data-step="%s"]' % index)
        check("step %s updates code, explanation and preview" % (index+1), js("document.querySelector('[aria-current=step]').dataset.step==='"+str(index)+"' && document.getElementById('detail-title').textContent.startsWith('"+str(index+1)+"') && document.getElementById('detail-code').textContent.length>40 && document.getElementById('fields').textContent.length>15"))
        if index == 2:
            check("Ethernet leads to offset 14", js("document.getElementById('offset').textContent.includes('14') && document.querySelector('.layer.active').dataset.layer==='ether'"))
        if index == 3:
            check("IPv4 leads to offset 34", js("document.getElementById('offset').textContent.includes('34') && document.querySelector('.layer.active').dataset.layer==='ip'"))
        if index == 4:
            check("UDP preview includes destination port 9000", js("document.getElementById('fields').textContent.includes('9000') && document.querySelector('.layer.active').dataset.layer==='udp'"))
    click('[data-step="4"]')
    click('[data-port="53"]')
    check("port switch updates bytes, input and extracted field", js("document.getElementById('input-port').textContent==='53' && document.getElementById('fields').textContent.includes(':53') && document.getElementById('detail-explain').textContent.includes('00 35')"))
    click('[data-step="2"]')
    check("earlier steps do not prematurely show extracted port", js("!document.getElementById('fields').textContent.includes(':53')"))
    click('[data-step="5"]')
    check("final return value is documented", js("document.getElementById('read').textContent.includes('DPPD_PARSE_OK = 0')"))
    click('#previous')
    check("previous navigation works", js("document.querySelector('[aria-current=step]').dataset.step==='4'"))
    click('#next')
    check("next navigation works", js("document.querySelector('[aria-current=step]').dataset.step==='5'"))
    check("end navigation disabled correctly", js("document.getElementById('next').disabled"))
    click('[data-port="9000"]')
    click('[data-step="0"]')
    check("start navigation disabled correctly", js("document.getElementById('previous').disabled"))
    check("previous lesson backlink resolves", js("document.querySelector('header a').href.endsWith('/workflow-software-packet-path-20261020-143318/software-packet-learning.html#step-2')"))
'''
script = script[:start] + checks + script[end:]
(root/"check-lesson-browser.py").write_text(script,encoding="utf-8")

