import json, pathlib, re
root=pathlib.Path(__file__).resolve().parent
previous=root.parent/"packet-path"/"software-packet-learning.html"
style=re.search(r"<style>(.*?)</style>",previous.read_text(encoding="utf-8"),re.S).group(1)
steps=json.loads((root/"section.json").read_text(encoding="utf-8"))
template=(root/"section-template.html").read_text(encoding="utf-8")
output=template.replace("__SHARED_STYLE__",style).replace("__SECTION_STEPS__",json.dumps(steps,ensure_ascii=False).replace("</",r"<\/"))
(root/"software-rules-learning.html").write_text(output,encoding="utf-8")
print(root/"software-rules-learning.html")

