#!/usr/bin/env python3
"""检查 DPDK 教材的本地引用与基本结构，并导出需要实际渲染的 Mermaid 源码"""

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import unquote, urlsplit


REPOSITORY = Path(__file__).resolve().parents[3]
TUTORIAL = REPOSITORY / "docs" / "dpdk"
EXPECTED_CHAPTERS = {
    "01-overview.md",
    "02-eal-devices.md",
    "03-memory-mbuf.md",
    "04-queues-io.md",
    "05-multicore-performance.md",
    "06-qsbr.md",
    "07-rte-flow.md",
    "08-smartnic-dpu.md",
    "09-async-templates.md",
    "10-observability-debugging.md",
}
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
# 教材使用普通内联链接和引用定义；允许尖括号路径及路径里的一层括号
LINK = re.compile(r"!?\[[^\]\n]*\]\(\s*(?:<([^>\n]+)>|([^\s()]+(?:\([^\n()]*\)[^\s()]*)*))"
                  r"(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*\)")
REFERENCE = re.compile(r"^ {0,3}\[[^\]]+\]:\s*(?:<([^>]+)>|(\S+))")


def relative(path):
    """报告使用仓库相对路径，方便在不同机器上查看同一处问题"""
    return path.relative_to(REPOSITORY).as_posix()


def check_document(path, issues, diagrams):
    """围栏内部只检查空白与配对，避免把实验命令误当成正文链接"""
    name = relative(path)
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        issues.append({"file": name, "kind": "read_error", "detail": str(error)})
        return {"file": name, "mermaid_count": 0, "readable": False}

    prose = []
    opened = None
    body = []
    diagram_count = 0
    local_links = 0

    for number, line in enumerate(lines, 1):
        if line != line.rstrip(" \t"):
            issues.append({"file": name, "line": number, "kind": "trailing_whitespace"})

        match = FENCE.match(line)
        if opened is not None:
            marker, language, start = opened
            if match and match[1][0] == marker[0] and len(match[1]) >= len(marker) and not match[2].strip():
                if language == "mermaid":
                    diagram_count += 1
                    identifier = re.sub(r"[^a-zA-Z0-9_-]", "-", path.relative_to(TUTORIAL).with_suffix("").as_posix())
                    diagrams.append({"id": f"{identifier}-{diagram_count:02d}", "file": name, "code": "\n".join(body)})
                opened = None
                body = []
            else:
                body.append(line)
            continue

        if match and not (match[1][0] == "`" and "`" in match[2]):
            info = match[2].strip().split()
            opened = (match[1], info[0].lower() if info else "", number)
            continue

        prose.append(line)
        # 行内代码中的链接语法通常是示例，不要求示例路径存在
        without_code = re.sub(r"(`+).*?\1", "", line)
        matches = list(LINK.finditer(without_code))
        reference = REFERENCE.match(without_code)
        if reference:
            matches.append(reference)
        for link in matches:
            destination = link[1] or link[2]
            if destination.startswith(("#", "//")):
                continue
            try:
                parsed = urlsplit(destination)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                decoded = unquote(parsed.path)
                target = (REPOSITORY / decoded.lstrip("/")) if decoded.startswith("/") else (path.parent / decoded)
                local_links += 1
                if not target.exists():
                    issues.append({"file": name, "line": number, "kind": "missing_local_link", "target": destination})
            except (OSError, ValueError) as error:
                issues.append({"file": name, "line": number, "kind": "invalid_link", "target": destination, "detail": str(error)})

    if opened is not None:
        issues.append({"file": name, "line": opened[2], "kind": "unclosed_fence", "marker": opened[0]})

    headings = "\n".join(line for line in prose if re.match(r"^ {0,3}#{1,6}\s", line))
    return {
        "file": name,
        "readable": True,
        "lines": len(lines),
        "local_links_checked": local_links,
        "mermaid_count": diagram_count,
        "has_diagram": diagram_count > 0 or any(re.search(r"!\[[^\]]*\]\(", line) for line in prose),
        "has_experiment": "实验" in headings,
        "has_self_check": "自检" in headings,
    }


def main():
    """输出仅描述静态检查，不把源码导出当作渲染成功或实验通过"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=REPOSITORY / "build/dpdk-learning/content-check.json",
                        help="检查报告路径；显式相对路径相对于当前工作目录")
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    issues = []
    diagrams = []
    documents = [check_document(path, issues, diagrams) for path in sorted(TUTORIAL.rglob("*.md"))]

    actual_chapters = {path.name for path in (TUTORIAL / "chapters").glob("*.md")}
    for missing in sorted(EXPECTED_CHAPTERS - actual_chapters):
        issues.append({"file": f"docs/dpdk/chapters/{missing}", "kind": "missing_chapter"})
    for extra in sorted(actual_chapters - EXPECTED_CHAPTERS):
        issues.append({"file": f"docs/dpdk/chapters/{extra}", "kind": "unexpected_chapter"})

    chapters = []
    for document in documents:
        if Path(document["file"]).parent.as_posix() != "docs/dpdk/chapters":
            continue
        chapters.append(document)
        for field in ("has_diagram", "has_experiment", "has_self_check"):
            if not document.get(field, False):
                issues.append({"file": document["file"], "kind": "missing_chapter_section", "section": field})

    output.parent.mkdir(parents=True, exist_ok=True)
    diagram_output = output.parent / "mermaid-input.json"
    report = {
        "ok": not issues,
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "静态检查本地链接、围栏、尾空白和章节结构；不代表图已渲染或实验已执行",
        "markdown_files": len(documents),
        "chapter_count": len(chapters),
        "mermaid_count": len(diagrams),
        "mermaid_input": diagram_output.name,
        "chapters": chapters,
        "documents": documents,
        "issues": issues,
    }
    diagram_output.write_text(json.dumps(diagrams, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": report["ok"], "markdown_files": len(documents), "chapters": len(chapters),
                      "mermaid_count": len(diagrams), "issue_count": len(issues), "output": str(output)}, ensure_ascii=False))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
