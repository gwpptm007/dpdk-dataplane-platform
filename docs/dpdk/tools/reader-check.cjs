"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {pathToFileURL, fileURLToPath} = require("node:url");
const {chromium} = require("playwright");

const root = path.resolve(__dirname, "../../..");
const reader = path.join(root, "docs/dpdk/reader/index.html");
const output = path.join(root, "build/dpdk-learning/reader");
const browserPath = process.env.DPDK_DOC_BROWSER;
const checks = [];

/** 记录实际通过的交互检查，异常会直接使脚本失败，避免把截图生成当成验收完成 */
function passed(name) {
  checks.push(name);
}

/** 每个可缩放图都检查文字边界，手机横向滚动容器不算页面溢出 */
async function inspectLayout(page) {
  const result = await page.evaluate(() => {
    const ids = [...document.querySelectorAll("[id]")].map(element => element.id);
    const duplicates = ids.filter((id, index) => ids.indexOf(id) !== index);
    const svgOverflow = [];
    for (const svg of document.querySelectorAll(".state-art")) {
      if (!svg.getBoundingClientRect().width) continue;
      for (const text of svg.querySelectorAll("text")) {
        const box = text.getBBox();
        const view = svg.viewBox.baseVal;
        if (box.x < 0 || box.y < 0 || box.x + box.width > view.width || box.y + box.height > view.height) {
          svgOverflow.push(text.textContent);
        }
      }
    }
    return {
      pageOverflow: document.documentElement.scrollWidth > innerWidth + 1,
      duplicates,
      svgOverflow
    };
  });
  assert.deepEqual(result, {pageOverflow: false, duplicates: [], svgOverflow: []});
}

async function main() {
  fs.mkdirSync(output, {recursive: true});
  const browser = await chromium.launch({headless: true, ...(browserPath ? {executablePath: browserPath} : {})});
  const errors = [];
  const requests = [];
  try {
    const context = await browser.newContext({viewport: {width: 1440, height: 1050}});
    /** 教材必须无需 CDN 即可打开，拦截所有远程请求来验证这一点 */
    await context.route(/^https?:\/\//, route => {
      requests.push(route.request().url());
      return route.abort();
    });
    const page = await context.newPage();
    page.on("pageerror", error => errors.push(error.message));
    await page.goto(pathToFileURL(reader).href);
    await page.waitForSelector("#step-tabs button");
    assert.equal(await page.locator("#step-tabs button").count(), 7);
    assert.equal(await page.locator("#previous").isDisabled(), true);
    assert.equal(await page.locator("#next").isDisabled(), false);
    await inspectLayout(page);
    await page.screenshot({path: path.join(output, "desktop-opening.png")});
    passed("离线初始加载、首步边界、桌面布局");

    const expectedNames = ["准备对象", "挂接到 RX", "DMA 搬运", "完成写回", "换缓冲", "burst 返回", "归还资源"];
    for (let index = 0; index < expectedNames.length; index++) {
      await page.locator("#step-tabs button").nth(index).click();
      assert.equal(await page.locator("#step-counter").textContent(), `第 ${index + 1} / 7 步`);
      assert.match(await page.locator("#step-announcement").textContent(), new RegExp(expectedNames[index]));
      assert.equal(await page.locator('#step-tabs button[aria-pressed="true"]').count(), 1);
      await inspectLayout(page);
      await page.locator("#walkthrough .lab").screenshot({path: path.join(output, `step-${index + 1}.png`)});
    }
    assert.equal(await page.locator("#next").isDisabled(), true);
    await page.locator("#previous").focus();
    await page.keyboard.press("Enter");
    assert.equal(await page.locator("#step-counter").textContent(), "第 6 / 7 步");
    await page.locator("#next").click();
    assert.equal(await page.locator("#step-counter").textContent(), "第 7 / 7 步");
    passed("七步状态、末步边界、按钮键盘操作与 SVG 文字范围");

    await page.locator("#toggle-mode").click();
    assert.equal(await page.locator("#continuous > article").count(), 7);
    assert.equal(await page.locator("#continuous").isVisible(), true);
    assert.equal(await page.locator("#step-panel").isVisible(), false);
    await inspectLayout(page);
    await page.locator("#toggle-mode").click();
    assert.equal(await page.locator("#step-counter").textContent(), "第 7 / 7 步");
    passed("全部展开与返回逐步模式、当前步骤保留、页面 ID 唯一");

    const answer = page.locator("#self-check details").first();
    await answer.locator("summary").focus();
    await page.keyboard.press("Space");
    assert.equal(await answer.getAttribute("open"), "");
    await page.keyboard.press("Space");
    assert.equal(await answer.getAttribute("open"), null);
    passed("参考答案键盘展开和收起");

    assert.match(await page.locator("#budget-result").textContent(), /2560/);
    await page.locator("#held").fill("3072");
    assert.match(await page.locator("#budget-result").textContent(), /余量为 0/);
    await page.locator("#held").fill("3073");
    assert.match(await page.locator("#budget-result").textContent(), /状态不成立.*多 1 个/);
    await page.locator("#held").fill("");
    assert.match(await page.locator("#budget-result").textContent(), /请输入/);
    await page.locator("#held").fill("1.5");
    assert.match(await page.locator("#budget-result").textContent(), /请输入/);
    await page.locator("#pool-size").fill("1");
    assert.match(await page.locator("#budget-result").textContent(), /请输入/);
    await page.locator("#pool-size").fill("4096");
    await page.locator("#held").fill("512");
    assert.match(await page.locator("#budget-result").textContent(), /2560/);
    passed("资源模型的正常、耗尽、不可能状态与输入合法性");

    /** 在真实 DOM 上找本地目标，避免导航目录或资源改名后出现失效入口 */
    const links = await page.evaluate(() => [...document.querySelectorAll("a[href], img[src], script[src], link[href]")]
      .map(element => ({url: element.href || element.src, tag: element.tagName})));
    for (const link of links) {
      const url = new URL(link.url);
      if (url.protocol !== "file:") continue;
      const target = fileURLToPath(url);
      assert.ok(fs.existsSync(target), `缺失本地目标：${link.url}`);
      if (target === reader && url.hash) {
        const id = decodeURIComponent(url.hash.slice(1));
        assert.ok(await page.evaluate(value => Boolean(document.getElementById(value)), id), `缺失锚点：${id}`);
      }
    }
    passed("本地链接、资源和章节锚点");

    for (const width of [390, 320]) {
      await page.setViewportSize({width, height: 844});
      await page.evaluate(() => scrollTo(0, 0));
      await inspectLayout(page);
      await page.screenshot({path: path.join(output, `mobile-${width}.png`)});
      await page.locator("#step-tabs button").nth(4).click();
      await inspectLayout(page);
      await page.locator("#walkthrough .lab").screenshot({path: path.join(output, `mobile-step-${width}.png`)});
    }
    passed("390px 与 320px 手机布局、图示横向滚动、页面不横向溢出");

    await page.setViewportSize({width: 1440, height: 1050});
    await page.emulateMedia({media: "print"});
    assert.equal(await page.locator("#continuous").isVisible(), true);
    assert.equal(await page.locator("#step-panel").isVisible(), false);
    assert.equal(await page.locator(".sidebar").isVisible(), false);
    await inspectLayout(page);
    passed("打印时自动显示全部推演步骤");

    const noScript = await browser.newContext({javaScriptEnabled: false, viewport: {width: 390, height: 844}});
    const plainPage = await noScript.newPage();
    await plainPage.goto(pathToFileURL(reader).href);
    assert.ok(await plainPage.locator("#question").isVisible());
    assert.match(await plainPage.locator("noscript").textContent(), /准备缓冲/);
    passed("禁用脚本后正文与接收因果链仍可阅读");
    assert.deepEqual(errors, []);
    assert.deepEqual(requests, []);
    passed("无浏览器脚本异常、无远程资源请求");

    const report = {
      ok: true, checked_at_utc: new Date().toISOString(), browser: browser.version(),
      scope: "离线 HTML 内容交互和布局检查；不代表 DPDK 运行或物理 NIC 验收",
      checks, page_errors: errors, remote_resource_requests: requests,
      screenshots: fs.readdirSync(output).filter(name => name.endsWith(".png")),
      visual_review: "请另行查看截图，不从自动化通过推断人工视觉验收"
    };
    fs.writeFileSync(path.join(output, "reader-check.json"), JSON.stringify(report, null, 2) + "\n");
    process.stdout.write(JSON.stringify({ok: true, checks: checks.length, output}, null, 2) + "\n");
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  process.stderr.write(`${error.stack}\n`);
  process.exitCode = 1;
});
