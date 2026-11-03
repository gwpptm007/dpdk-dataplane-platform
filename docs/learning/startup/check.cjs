/* Validate the startup learning page without running DPDK or rewriting historical reports. */
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const net = require('node:net');
const {spawn} = require('node:child_process');
const {pathToFileURL, fileURLToPath} = require('node:url');
const ROOT = path.resolve(__dirname, '../../..');
const report = {scope:'Startup guide, directory navigation, source anchors and responsive rendering', passed:false, checks:[], runtimeErrors:[], screenshots:[]};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
function check(name, value) { report.checks.push({name, passed:!!value}); if (!value) throw Error(name); }
function localLinks(file) {
  const text = fs.readFileSync(file, 'utf8');
  const matches = [...text.matchAll(/(?:href|src)="([^"]+)"/g)];
  for (const [,href] of matches) {
    const url = new URL(href, pathToFileURL(file));
    if (url.protocol !== 'file:') continue;
    const target = fileURLToPath(url);
    check(`Link exists: ${path.relative(ROOT,target)}`, fs.existsSync(target));
    if (url.hash && target.endsWith('.html') && (target.includes(`${path.sep}sources${path.sep}`) || target.startsWith(__dirname))) {
      check(`Anchor exists: ${url.hash}`, fs.readFileSync(target,'utf8').includes(`id="${decodeURIComponent(url.hash.slice(1))}"`));
    }
  }
}
async function main() {
  const page = path.join(__dirname,'index.html');
  const directory = path.resolve(__dirname,'../index.html');
  localLinks(page); localLinks(directory);
  const chrome = [process.env.PROGRAMFILES && path.join(process.env.PROGRAMFILES,'Google/Chrome/Application/chrome.exe'), process.env['PROGRAMFILES(X86)'] && path.join(process.env['PROGRAMFILES(X86)'],'Microsoft/Edge/Application/msedge.exe'), '/usr/bin/google-chrome','/usr/bin/chromium'].find(p=>p && fs.existsSync(p));
  if (!chrome) throw Error('Chrome/Edge is required for browser checks');
  const server=net.createServer(); await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const port=server.address().port; await new Promise(resolve=>server.close(resolve));
  const profile=fs.mkdtempSync(path.join(os.tmpdir(),'dppd-startup-guide-'));
  const proc=spawn(chrome,['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check',`--remote-debugging-port=${port}`,`--user-data-dir=${profile}`,'about:blank'],{windowsHide:true,stdio:'ignore'});
  let ws;
  try {
    let targets;
    for(let i=0;i<150;i++) {
      try { targets=await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if(targets.some(t=>t.type==='page')) break; } catch {}
      await pause(100);
    }
    const target=targets?.find(t=>t.type==='page'); if(!target) throw Error('Browser startup timed out');
    ws=new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});});
    const pending=new Map(); let sequence=0;
    ws.addEventListener('message',event=>{const data=JSON.parse(event.data);if(data.method==='Runtime.exceptionThrown')report.runtimeErrors.push(data.params);if(pending.has(data.id)){const request=pending.get(data.id);pending.delete(data.id);clearTimeout(request.timer);data.error?request.reject(Error(JSON.stringify(data.error))):request.resolve(data.result);}});
    function cdp(method,params={}) { return new Promise((resolve,reject)=>{const id=++sequence;const timer=setTimeout(()=>{pending.delete(id);reject(Error(`CDP timeout: ${method}`));},15000);pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params}));}); }
    async function js(expression) {const value=await cdp('Runtime.evaluate',{expression,returnByValue:true});if(value.exceptionDetails)throw Error(JSON.stringify(value.exceptionDetails));return value.result.value;}
    async function ready(url) {for(let i=0;i<100;i++){if(await js(`location.href===${JSON.stringify(url)}&&document.readyState==='complete'`))return;await pause(40);}throw Error(`Navigation timeout: ${url}`);}
    async function open(file,fragment='') {const url=pathToFileURL(file).href+fragment;await cdp('Page.navigate',{url});await ready(url);}
    await cdp('Page.enable');await cdp('Runtime.enable');
    await open(directory);
    check('Directory points to the restart of learning',await js("document.querySelector('#resume-learning').getAttribute('href')==='startup/index.html#stage-0'"));
    check('Directory does not claim mastery',await js("document.body.innerText.includes('尚未确认理解')"));
    await js("document.querySelector('#resume-learning').click()");await ready(pathToFileURL(page).href+'#stage-0');
    check('Directory button opens startup guide',await js("document.querySelectorAll('.step-card').length===6"));
    await open(page);
    check('All six explanations visible without clicking',await js("[...document.querySelectorAll('.step-card')].every(e=>getComputedStyle(e).display!=='none'&&e.getBoundingClientRect().height>0)"));
    check('Only code is initially collapsed',await js("document.querySelectorAll('details').length===6&&[...document.querySelectorAll('details')].every(d=>!d.open)"));
    await js("document.querySelector('#toggle-code').click()");await pause(80);
    check('Expand all source excerpts',await js("[...document.querySelectorAll('.source-detail')].every(d=>d.open)&&document.querySelector('#toggle-code').getAttribute('aria-pressed')==='true'"));
    await js("document.querySelector('#toggle-code').click()");await pause(80);
    check('Collapse all source excerpts',await js("[...document.querySelectorAll('.source-detail')].every(d=>!d.open)"));
    for(let stage=0;stage<6;stage++) {await js(`document.querySelector('#stage-nav a[href="#stage-${stage}"]').click()`);await pause(40);check(`Stage ${stage} navigation`,await js(`document.querySelector('#stage-nav [aria-current="step"]').getAttribute('href')==='#stage-${stage}'`));}
    const source=await js("document.querySelector('.code-block .source a').href");
    await cdp('Page.navigate',{url:source});await ready(source);
    check('Source page scrolls to the current line',await js("!!document.querySelector(':target')&&document.querySelector(':target').innerText.includes('rte_eal_init')"));
    await open(page);
    for (const width of [1440,800,390]) {
      await cdp('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:false});await pause(80);
      check(`No horizontal overflow at ${width}px`,await js('document.documentElement.scrollWidth<=innerWidth'));
      check(`Main explanations readable at ${width}px`,await js("parseFloat(getComputedStyle(document.querySelector('.step-card>p')).fontSize)>=16"));
      if(width!==800){await js("document.documentElement.style.scrollBehavior='auto';scrollTo(0,0)");await pause(50);const shot=await cdp('Page.captureScreenshot',{format:'png'});const name=`preview-${width}.png`;fs.writeFileSync(path.join(__dirname,name),Buffer.from(shot.data,'base64'));report.screenshots.push(name);}
    }
    check('No browser script exceptions',report.runtimeErrors.length===0);
    report.passed=true;
  } finally {
    if(ws)ws.close();proc.kill();
    await Promise.race([new Promise(resolve=>proc.once('exit',resolve)),pause(1500)]);
    const resolved=path.resolve(profile),base=path.resolve(os.tmpdir());
    if(path.dirname(resolved)!==base||!path.basename(resolved).startsWith('dppd-startup-guide-'))throw Error('Refused unsafe browser-profile cleanup');
    fs.rmSync(resolved,{recursive:true,force:true,maxRetries:5,retryDelay:100});
  }
}
main().catch(error=>{report.error=String(error.stack||error);process.exitCode=1;}).finally(()=>{fs.writeFileSync(path.join(__dirname,'check.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify({passed:report.passed,checks:report.checks.length,report:'docs/learning/startup/check.json',error:report.error}));});
