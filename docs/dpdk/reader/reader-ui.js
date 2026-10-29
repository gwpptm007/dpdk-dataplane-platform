"use strict";

/** 两章共用阅读操作，具体图形和状态文案由各章提供，避免控件行为逐渐不一致 */
window.DpdkReader = (() => {
  function mountSteps(steps, renderDiagram) {
    let current = 0;
    const panel = document.getElementById("step-panel");
    const tabs = document.getElementById("step-tabs");
    const previous = document.getElementById("previous");
    const next = document.getElementById("next");
    const toggle = document.getElementById("toggle-mode");
    const continuous = document.getElementById("continuous");

    /** 文案只来自仓库中的固定内容，读者输入不会作为 HTML 插进页面 */
    function explain(step, index, unique) {
      return `${renderDiagram(step, unique)}<div class="state-copy"><h3>${index + 1}. ${step.title}</h3>
        <p>${step.text}</p><dl><dt>谁在做</dt><dd>${step.actor}</dd><dt>改了什么</dt><dd>${step.changed}</dd>
        <dt>谁能使用</dt><dd>${step.owner}</dd></dl><p><strong>为什么需要这一步：</strong>${step.why}</p></div>`;
    }

    /** 只在读者操作后切换步骤，不自动播放，也不把翻页当作已经掌握 */
    function show(index) {
      current = Math.max(0, Math.min(steps.length - 1, index));
      panel.innerHTML = explain(steps[current], current, "current");
      [...tabs.children].forEach((button, i) => button.setAttribute("aria-pressed", String(i === current)));
      previous.disabled = current === 0;
      next.disabled = current === steps.length - 1;
      document.getElementById("step-counter").textContent = `第 ${current + 1} / ${steps.length} 步`;
      document.getElementById("step-announcement").textContent = `第 ${current + 1} 步：${steps[current].name}`;
    }

    steps.forEach((step, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = `${index + 1} ${step.name}`;
      button.setAttribute("aria-controls", "step-panel");
      button.addEventListener("click", () => show(index));
      tabs.append(button);
      const article = document.createElement("article");
      article.innerHTML = explain(step, index, `all-${index}`);
      continuous.append(article);
    });
    previous.addEventListener("click", () => show(current - 1));
    next.addEventListener("click", () => show(current + 1));
    toggle.addEventListener("click", () => {
      const all = continuous.hidden;
      continuous.hidden = !all;
      panel.hidden = all;
      tabs.hidden = all;
      document.getElementById("step-controls").hidden = all;
      toggle.textContent = all ? "切回逐步推演" : "展开全部步骤";
      toggle.setAttribute("aria-expanded", String(all));
    });
    show(0);
  }

  /** 目录只标出当前阅读位置，不写入个人学习记录 */
  if ("IntersectionObserver" in window) {
    const observer = new IntersectionObserver(entries => {
      const visible = entries.find(entry => entry.isIntersecting);
      if (!visible) return;
      document.querySelectorAll(".sidebar nav a").forEach(link => {
        if (link.getAttribute("href") === `#${visible.target.id}`) link.setAttribute("aria-current", "location");
        else link.removeAttribute("aria-current");
      });
    }, {rootMargin: "-10% 0px -65% 0px"});
    document.querySelectorAll("main section[id]").forEach(section => observer.observe(section));
  }
  return Object.freeze({mountSteps});
})();
