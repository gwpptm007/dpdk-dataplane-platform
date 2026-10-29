/** 在真实浏览器中逐张渲染教材 Mermaid，保存 SVG、截图和检查结果 */
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

async function main() {
  const [input, bundle, output = 'build/dpdk-learning/rendered'] = process.argv.slice(2);
  if (!input || !bundle) throw new Error('用法：node render-check.cjs mermaid-input.json mermaid.min.js [输出目录]');
  const diagrams = JSON.parse(fs.readFileSync(input, 'utf8'));
  fs.mkdirSync(output, { recursive: true });
  const browser = await chromium.launch({ headless: true,
    ...(process.env.DPDK_DOC_BROWSER ? { executablePath: process.env.DPDK_DOC_BROWSER } : {}) });
  const results = [];
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1 });
    await page.setContent('<html><head><meta charset="utf-8"></head><body><main></main></body></html>');
    await page.addScriptTag({ path: path.resolve(bundle) });
    await page.evaluate(() => mermaid.initialize({ startOnLoad: false, securityLevel: 'strict',
      theme: 'default', fontFamily: 'Microsoft YaHei, sans-serif' }));
    for (const [index, item] of diagrams.entries()) {
      try {
        const rendered = await page.evaluate(async ({ code, index }) => {
          const main = document.querySelector('main');
          main.innerHTML = '';
          const result = await mermaid.render(`diagram_${index}`, code);
          main.innerHTML = result.svg;
          const svg = main.querySelector('svg');
          svg.style.maxWidth = '100%';
          svg.style.height = 'auto';
          const box = svg.getBoundingClientRect();
          return { svg: svg.outerHTML, width: box.width, height: box.height,
            overflow: document.documentElement.scrollWidth > innerWidth };
        }, { code: item.code, index });
        if (rendered.width <= 0 || rendered.height <= 0 || rendered.overflow) throw new Error('图没有可见尺寸或横向溢出');
        fs.writeFileSync(path.join(output, `${item.id}.svg`), rendered.svg);
        await page.locator('main').screenshot({ path: path.join(output, `${item.id}.png`) });
        results.push({ id: item.id, file: item.file, ok: true, width: rendered.width, height: rendered.height });
      } catch (error) {
        results.push({ id: item.id, file: item.file, ok: false, error: String(error) });
      }
    }
  } finally {
    await browser.close();
  }
  const report = { renderer: 'Mermaid browser bundle supplied by caller',
    ok: results.every(item => item.ok), diagrams: results.length, results };
  fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify({ ok: report.ok, diagrams: report.diagrams, failures: results.filter(item => !item.ok) }));
  if (!report.ok) process.exitCode = 1;
}

main().catch(error => { console.error(error); process.exitCode = 1; });
