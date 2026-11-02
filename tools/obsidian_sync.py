#!/usr/bin/env python3
"""Two-way sync of PULHRA's docs with an Obsidian vault folder (Tim, 2.11; run on request: "синхронизируй").

  obsidian_sync.py SRC VAULT [--date YYYY-MM-DD] [--prefix PULHRA/]

SRC   the docs as they are now, by relative path (e.g. Проект/PULHRA2-SPEC.md = the Claude project's claude/PULHRA2-SPEC.md,
      Репо/PULHRA.md = the repo's PULHRA.md); the caller fills it, and takes back the files this script rewrites there.
VAULT a copy of the vault's PULHRA folder; rewritten in place (the caller writes it back to the Mac).

A note: frontmatter, then the doc between <!-- auto:start --> and <!-- auto:end --> (synced both ways), then
"## Мои заметки" (Obsidian's only, never synced). VAULT/.sync-state.json keeps each doc's sha at the last sync:
  only SRC changed      -> the note's auto block rewritten
  only the note changed -> SRC rewritten from the note (the caller pushes it to the project / commits it)
  both, differently     -> SRC wins in the note, the note's version kept beside it as "<name> (конфликт <date>).md"
  a new note in VAULT (no markers, not in the state) -> a new doc in SRC (Проект/ when at the folder's top)
Deletions are not synced, only reported. Prints a JSON report: {"to_src": [...], "to_vault": [...], "conflicts": [...],
"new": [...], "gone": [...]}.
"""
import datetime
import hashlib
import json
import os
import re
import sys

START = "<!-- auto:start — двусторонняя синхронизация с Claude-проектом и репо PULHRA: правь где угодно, изменения уйдут обратно -->"
END = "<!-- auto:end -->"
MINE = "## Мои заметки"
STATE = ".sync-state.json"
INDEX = "PULHRA.md"     # the folder's index note (not a doc)


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def norm(s):
    return s.replace("\r\n", "\n").strip("\n") + "\n"


def split_note(text):
    """-> (auto body or None, my notes)"""
    i = text.find("<!-- auto:start")
    j = text.find(END)
    if i < 0 or j < i:
        return None, ""
    body = text[text.index("-->", i) + 3:j]
    rest = text[j + len(END):]
    k = rest.find(MINE)
    mine = rest[k + len(MINE):].strip("\n") if k >= 0 else rest.strip("\n")
    return norm(body), mine


def strip_front(text):
    if text.startswith("---\n"):
        k = text.find("\n---", 4)
        if k > 0:
            return text[k + 4:]
    return text


def make_note(rel, body, mine, date):
    tag = rel.split("/")[0].lower()
    return (f"---\ntags: [pulhra, {tag}]\nsynced: {date}\nsource: {rel}\n---\n{START}\n\n{body.rstrip()}\n\n{END}\n\n"
            f"{MINE}\n\n{mine.strip()}\n" if mine.strip() else
            f"---\ntags: [pulhra, {tag}]\nsynced: {date}\nsource: {rel}\n---\n{START}\n\n{body.rstrip()}\n\n{END}\n\n{MINE}\n\n")


def walk(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(".md"):
                rel = os.path.relpath(os.path.join(d, f), root).replace(os.sep, "/")
                out[rel] = open(os.path.join(d, f), encoding="utf-8").read()
    return out


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p) or root, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    src, vault = sys.argv[1], sys.argv[2]
    date = sys.argv[sys.argv.index("--date") + 1] if "--date" in sys.argv else datetime.date.today().isoformat()
    pre = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "PULHRA/"   # the folder in the vault
    sp = os.path.join(vault, STATE)
    state = json.load(open(sp, encoding="utf-8")) if os.path.exists(sp) else {"notes": {}}
    notes = state.setdefault("notes", {})
    docs = {k: norm(v) for k, v in walk(src).items()}
    vnotes = {k: v for k, v in walk(vault).items() if k != INDEX and "(конфликт " not in k}
    rep = {"to_src": [], "to_vault": [], "conflicts": [], "new": [], "gone": []}

    for rel, text in list(vnotes.items()):                 # notes made in Obsidian: new docs
        if rel in docs or rel in notes:
            continue
        body, _ = split_note(text)
        if body is None:
            body = norm(strip_front(text))
        r = rel if "/" in rel else "Проект/" + rel
        docs[r] = body
        write(src, r, body)
        rep["new"].append(r)
        if r != rel:
            os.remove(os.path.join(vault, rel))
            vnotes.pop(rel)
        else:
            vnotes[rel] = ""                               # rewritten below with markers

    for rel, body in sorted(docs.items()):
        st = notes.get(rel, {})
        vt = vnotes.get(rel)
        vbody, mine = split_note(vt) if vt else (None, "")
        if vbody is None:                                  # no note yet (or no markers): the doc into it
            write(vault, rel, make_note(rel, body, mine, date))
            if rel not in rep["new"]:
                rep["to_vault"].append(rel)
            notes[rel] = {"src_sha": sha(body), "auto_sha": sha(body)}
            continue
        s_ch = sha(body) != st.get("src_sha")
        v_ch = sha(vbody) != st.get("auto_sha")
        if vbody == body:
            pass
        elif v_ch and not s_ch:
            body = vbody
            write(src, rel, body)
            rep["to_src"].append(rel)
        elif s_ch and v_ch:
            base, ext = os.path.splitext(rel)
            write(vault, f"{base} (конфликт {date}){ext}", vt)
            rep["conflicts"].append(rel)
        else:
            rep["to_vault"].append(rel)
        write(vault, rel, make_note(rel, body, mine, date))
        notes[rel] = {"src_sha": sha(body), "auto_sha": sha(body)}

    rep["gone"] = sorted(r for r in notes if r not in docs)
    state["last_sync"] = date
    idx = ["---", "tags: [pulhra, index]", f"synced: {date}", "---", START, "",
           "# PULHRA — прошивка для M-VAVE FM-1", "",
           "Синхронизация с Claude-проектом «Кастомные Прошивки Для Mwave Fm1» и репо 646timur-cell/pulhra_main — по команде «синхронизируй».", ""]
    for grp in ("Проект", "Репо"):
        items = sorted(r for r in docs if r.startswith(grp + "/"))
        if items:
            idx += [f"## {grp}", ""] + [f"- [[{pre}{r[:-3]}|{os.path.basename(r)[:-3]}]]" for r in items] + [""]
    idx += [f"Последняя синхронизация: {date}", "", END, "", MINE, ""]
    old = open(os.path.join(vault, INDEX), encoding="utf-8").read() if os.path.exists(os.path.join(vault, INDEX)) else ""
    _, mine = split_note(old)
    write(vault, INDEX, "\n".join(idx) + ("\n" + mine.strip() + "\n" if mine.strip() else ""))
    json.dump(state, open(sp, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(json.dumps(rep, ensure_ascii=False))


if __name__ == "__main__":
    main()
