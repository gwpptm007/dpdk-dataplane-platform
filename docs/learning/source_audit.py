import hashlib, json, pathlib, re, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
OUT=pathlib.Path(__file__).resolve().parent
TOKEN=re.compile(r'(?P<comment>/\*[\s\S]*?\*/|//[^\r\n]*)|(?P<literal>"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|(?P<word>[A-Za-z_]\w*|(?:\d[\w.]*))|(?P<other>[^\s])')
def tokens(text):
    return [m.group(0) for m in TOKEN.finditer(text) if m.lastgroup!="comment"]
def digest(data):
    return hashlib.sha256(data).hexdigest()
def sources():
    return sorted(p for base in ("lib","include","app","apps") if (ROOT/base).exists() for p in (ROOT/base).rglob("*") if p.suffix in (".c",".h"))
def state():
    return {p.relative_to(ROOT).as_posix():{"sha256":digest(p.read_bytes()),"tokens":digest(json.dumps(tokens(p.read_text(encoding="utf-8")),ensure_ascii=False).encode())} for p in sources()}
if __name__=="__main__":
    import sys
    target=OUT/"source-baseline.json"
    if sys.argv[1]=="baseline":
        if target.exists(): raise SystemExit("Baseline already exists; will not overwrite")
        target.write_text(json.dumps({"files":state(),"git_status":subprocess.check_output(["git","status","--short"],cwd=ROOT,text=True)},ensure_ascii=False,indent=2),encoding="utf-8")
        print("Captured source baseline",len(state()))
    elif sys.argv[1]=="check":
        before=json.loads(target.read_text(encoding="utf-8"))["files"]; after=state()
        assert before.keys()==after.keys(),"Source file set changed"
        changed=[p for p in before if before[p]["sha256"]!=after[p]["sha256"]]
        bad=[p for p in before if before[p]["tokens"]!=after[p]["tokens"]]
        report={"passed":not bad,"scope":"Compared with start of this Goal; pre-existing changes preserved","changed_files":changed,"noncomment_changes":bad,"source_count":len(after)}
        (OUT/"source-check.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
        print(json.dumps(report,ensure_ascii=False)); assert not bad

