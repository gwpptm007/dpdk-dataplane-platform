"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const {pathToFileURL, fileURLToPath} = require("node:url");
const {chromium} = require("playwright");

const root = path.resolve(__dirname, "../../..");
const reader = path.join(root, "docs/dpdk/reader/tx.html");
const output = path.join(root, "build/dpdk-learning/tx-reader");
const checks = [];
const screenshots = [];

/** 每项结论都在对应断言通过后记录，失败时不写出成功报告 */
function passed(name) {
  checks.push(name);
}

/** 只记录本次实际生成的截图，避免把旧目录里的图片误列成本次证据 */
async function capture(target, name) {
  await target.screenshot({path: path.join(output, name)});
  screenshots.push(name);
}

/** 图本身可以在小屏中滚动，但页面不能被撑宽，图中文字也不能超出画布 */
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

/** 完成数是驱动持有对象的子集，不能再加一次，否则会把同一份资源重复计数 */
async function expectResources(page, expected) {
  const actual = await page.evaluate(() => {
    const value = id => Number(document.getElementById(id).textContent.trim());
    return {
      app: value("app-count"), driver: value("driver-count"),
      done: value("done-count"), pool: value("pool-count")
    };
  });
  assert.deepEqual(actual, expected);
  assert.equal(actual.app + actual.driver + actual.pool, 8);
  assert.ok(actual.done >= 0 && actual.done <= actual.driver);
  assert.ok(Object.values(actual).every(value => Number.isInteger(value) && value >= 0));
}

/** 用同一个过程核对零接收、部分接收和全接收，避免只证明默认的三个包场景 */
async function exerciseAccepted(page, accepted, saveScreenshots = false) {
  await page.locator("#reset-tx").click();
  await expectResources(page, {app: 5, driver: 0, done: 0, pool: 3});
  assert.equal(await page.locator("#accepted").isDisabled(), false);
  assert.equal(await page.locator("#complete-tx").isDisabled(), true);
  assert.equal(await page.locator("#reclaim-tx").isDisabled(), true);

  await page.locator("#accepted").fill(String(accepted));
  await page.locator("#submit-tx").click();
  await expectResources(page, {app: 0, driver: accepted, done: 0, pool: 8 - accepted});
  assert.equal(await page.locator("#accepted").isDisabled(), true);
  assert.equal(await page.locator("#submit-tx").isDisabled(), true);
  assert.equal(await page.locator("#reclaim-tx").isDisabled(), true);
  assert.equal(await page.locator("#complete-tx").isDisabled(), accepted === 0);
  assert.ok((await page.locator("#exercise-status").textContent()).trim().length > 0);
  if (saveScreenshots) await capture(page.locator("#tx-exercise"), `accepted-${accepted}.png`);

  if (accepted === 0) return;
  await page.locator("#complete-tx").click();
  await expectResources(page, {app: 0, driver: accepted, done: accepted, pool: 8 - accepted});
  assert.equal(await page.locator("#complete-tx").isDisabled(), true);
  assert.equal(await page.locator("#reclaim-tx").isDisabled(), false);
  if (saveScreenshots) await capture(page.locator("#tx-exercise"), `completed-${accepted}.png`);

  await page.locator("#reclaim-tx").click();
  await expectResources(page, {app: 0, driver: 0, done: 0, pool: 8});
  assert.equal(await page.locator("#complete-tx").isDisabled(), true);
  assert.equal(await page.locator("#reclaim-tx").isDisabled(), true);
  assert.equal(await page.locator("#submit-tx").isDisabled(), true);
  if (saveScreenshots) await capture(page.locator("#tx-exercise"), `reclaimed-${accepted}.png`);
}

/** 检查本地资源存在，以及 HTML 章节链接的目标 ID 是否仍然有效 */
async function inspectLinks(page) {
  const links = await page.evaluate(() => [...document.querySelectorAll("a[href], img[src], script[src], link[href]")]
    .map(element => element.href || element.src));
  for (const href of links) {
    const url = new URL(href);
    if (url.protocol !== "file:") continue;
    const target = fileURLToPath(url);
    assert.ok(fs.existsSync(target), `缺失本地目标：${href}`);
    if (!url.hash || !target.endsWith(".html")) continue;
    const id = decodeURIComponent(url.hash.slice(1));
    if (target === reader) {
      assert.ok(await page.evaluate(value => Boolean(document.getElementById(value)), id), `缺失锚点：${href}`);
    } else {
      const ids = [...fs.readFileSync(target, "utf8").matchAll(/\bid\s*=\s*["']([^"']+)["']/g)]
        .map(match => match[1]);
      assert.ok(ids.includes(id), `缺失跨页锚点：${href}`);
    }
  }
}

async function main() {
  fs.mkdirSync(output, {recursive: true});
  /** 清掉上一轮报告，防止本轮失败时遗留的成功记录被误读 */
  fs.rmSync(path.join(output, "tx-reader-check.json"), {force: true});
  const browserPath = process.env.DPDK_DOC_BROWSER;
  const browser = await chromium.launch({headless: true, ...(browserPath ? {executablePath: browserPath} : {})});
  const errors = [];
  const requests = [];
  try {
    /** 拦截所有网络资源，使离线通过不依赖浏览器缓存中的远程脚本 */
    async function blockNetwork(context) {
      await context.route(/^https?:\/\//, route => {
        requests.push(route.request().url());
        return route.abort();
      });
    }
    const context = await browser.newContext({viewport: {width: 1440, height: 1050}});
    await blockNetwork(context);
    const page = await context.newPage();
    page.on("pageerror", error => errors.push(error.message));
    await page.goto(pathToFileURL(reader).href);
    await page.waitForSelector("#step-tabs button");
    assert.equal(await page.locator("#step-tabs button").count(), 7);
    assert.equal(await page.locator("#previous").isDisabled(), true);
    assert.equal(await page.locator("#next").isDisabled(), false);
    await expectResources(page, {app: 5, driver: 0, done: 0, pool: 3});
    assert.equal(await page.locator("#accepted").inputValue(), "3");
    await inspectLayout(page);
    await capture(page, "desktop-opening.png");
    passed("离线加载、初始资源、首步边界与桌面布局");

    const names = ["应用持有", "检查空间", "填写描述符", "发布并返回", "DMA读取", "完成写回", "驱动回收"];
    for (let index = 0; index < names.length; index++) {
      await page.locator("#step-tabs button").nth(index).click();
      assert.equal(await page.locator("#step-counter").textContent(), `第 ${index + 1} / 7 步`);
      const announcement = (await page.locator("#step-announcement").textContent()).replace(/\s+/g, "");
      assert.ok(announcement.includes(names[index]));
      assert.equal(await page.locator('#step-tabs button[aria-pressed="true"]').count(), 1);
      await inspectLayout(page);
      await capture(page.locator("#tx-walkthrough .lab"), `step-${index + 1}.png`);
    }
    assert.equal(await page.locator("#next").isDisabled(), true);
    await page.locator("#previous").focus();
    await page.keyboard.press("Enter");
    assert.equal(await page.locator("#step-counter").textContent(), "第 6 / 7 步");
    await page.locator("#next").click();
    assert.equal(await page.locator("#step-counter").textContent(), "第 7 / 7 步");
    passed("七步状态、末步边界、键盘操作与 SVG 文字范围");

    await page.locator("#toggle-mode").click();
    assert.equal(await page.locator("#continuous > article").count(), 7);
    assert.equal(await page.locator("#continuous").isVisible(), true);
    assert.equal(await page.locator("#step-panel").isVisible(), false);
    await inspectLayout(page);
    await page.locator("#toggle-mode").click();
    assert.equal(await page.locator("#step-counter").textContent(), "第 7 / 7 步");
    passed("全部展开和切回、步骤保留与 ID 唯一");

    const answer = page.locator("#tx-check details").first();
    await answer.locator("summary").focus();
    await page.keyboard.press("Space");
    assert.equal(await answer.getAttribute("open"), "");
    await page.keyboard.press("Space");
    assert.equal(await answer.getAttribute("open"), null);
    passed("自检答案可用键盘展开和收起");

    for (const accepted of [0, 1, 2, 3, 4, 5]) {
      await exerciseAccepted(page, accepted, [0, 3, 5].includes(accepted));
    }
    passed("0 到 5 个接受包的状态迁移、完成与归还分离、资源守恒及重复操作禁用");

    for (const value of ["", "-1", "6", "1.5"]) {
      await page.locator("#reset-tx").click();
      await page.locator("#accepted").fill(value);
      if (await page.locator("#submit-tx").isEnabled()) await page.locator("#submit-tx").click();
      await expectResources(page, {app: 5, driver: 0, done: 0, pool: 3});
      assert.equal(await page.locator("#accepted").isDisabled(), false);
      assert.equal(await page.locator("#complete-tx").isDisabled(), true);
      assert.equal(await page.locator("#reclaim-tx").isDisabled(), true);
    }
    await page.locator("#reset-tx").click();
    await expectResources(page, {app: 5, driver: 0, done: 0, pool: 3});
    passed("空值、负数、超限和小数不改变资源状态，重置恢复初始状态");

    await inspectLinks(page);
    passed("本地资源、目录锚点和跨页 HTML 锚点");

    for (const width of [390, 320]) {
      await page.setViewportSize({width, height: 844});
      await page.evaluate(() => scrollTo(0, 0));
      await inspectLayout(page);
      await capture(page, `mobile-${width}.png`);
      await page.locator("#step-tabs button").nth(4).click();
      await inspectLayout(page);
      await capture(page.locator("#tx-walkthrough .lab"), `mobile-step-${width}.png`);
      await exerciseAccepted(page, 3);
      await inspectLayout(page);
      await capture(page.locator("#tx-exercise"), `mobile-exercise-${width}.png`);
    }
    passed("390px 和 320px 手机的正文、图示与状态练习无页面横向溢出");

    await page.setViewportSize({width: 1440, height: 1050});
    await page.emulateMedia({media: "print"});
    assert.equal(await page.locator("#continuous").isVisible(), true);
    assert.equal(await page.locator("#step-panel").isVisible(), false);
    assert.equal(await page.locator(".sidebar").isVisible(), false);
    await inspectLayout(page);
    passed("打印时显示全部推演并隐藏交互导航");

    const noScript = await browser.newContext({javaScriptEnabled: false, viewport: {width: 390, height: 844}});
    await blockNetwork(noScript);
    const plainPage = await noScript.newPage();
    await plainPage.goto(pathToFileURL(reader).href);
    assert.ok(await plainPage.locator("#tx-question").isVisible());
    const fallback = (await plainPage.locator("noscript").allTextContents()).join(" ");
    assert.match(fallback, /提交|发送/);
    passed("禁用脚本后正文和发送因果链仍可阅读");

    assert.deepEqual(errors, []);
    assert.deepEqual(requests, []);
    passed("无浏览器脚本异常和远程资源请求");
    const sourceHashes = {};
    for (const name of ["tx.html", "reader-ui.js", "tx-lesson.js", "lesson.css"]) {
      sourceHashes[`docs/dpdk/reader/${name}`] = crypto.createHash("sha256")
        .update(fs.readFileSync(path.join(root, "docs/dpdk/reader", name))).digest("hex");
    }
    const report = {
      ok: true, checked_at_utc: new Date().toISOString(), browser: browser.version(),
      scope: "离线 TX 教学页面的交互和布局检查；不代表 DPDK 运行或真实 NIC 验收",
      checks, page_errors: errors, remote_resource_requests: requests,
      source_hashes: sourceHashes, screenshots,
      visual_review: "请另行查看截图，不从自动化通过推断人工视觉验收"
    };
    fs.writeFileSync(path.join(output, "tx-reader-check.json"), JSON.stringify(report, null, 2) + "\n");
    process.stdout.write(JSON.stringify({ok: true, checks: checks.length, output}, null, 2) + "\n");
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  process.stderr.write(`${error.stack}\n`);
  process.exitCode = 1;
});
