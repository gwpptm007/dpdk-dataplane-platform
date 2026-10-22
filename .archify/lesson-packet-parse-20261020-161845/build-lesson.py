import json, pathlib, re
root = pathlib.Path(__file__).resolve().parent
previous = root.parent / "workflow-software-packet-path-20261020-143318" / "software-packet-learning.html"
style = re.search(r"<style>(.*?)</style>", previous.read_text(encoding="utf-8"), re.S).group(1)
steps = json.loads((root / "lesson.json").read_text(encoding="utf-8"))
template = (root / "lesson-template.html").read_text(encoding="utf-8")
output = template.replace("__SHARED_STYLE__", style).replace("__LESSON_STEPS__",json.dumps(steps,ensure_ascii=False).replace("</", r"<\/"))
(root/"packet-parse-learning.html").write_text(output,encoding="utf-8")
print(root/"packet-parse-learning.html")

