#!/usr/bin/env python3
"""Extract quest -> step -> event-flag mappings from erquestlog (MIT, TheHaist).

Parses research/EldenRingQuestLog/src/erquestlog_quests.hpp (the ESD menu
definitions: ADD_TALK_LIST_*_DATA_ARGS entries whose conditions are
esd_get_flag(<event flag id>) expressions) and joins the display text from the
shipped english.lang, producing data/quests_flags.json -- the seed data for
QuestPath's own quest database.

Message id scheme (from erquestlog):
  87 QQ 00 00   quest title        (QQ = quest ordinal)
  87 QQ SS 00   step title         (SS = step ordinal, 99 = quest-dead entry)
  87 QQ SS 01   step description
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QUESTS_HPP = ROOT / "research/EldenRingQuestLog/src/erquestlog_quests.hpp"
ENGLISH_LANG = ROOT / "research/EldenRingQuestLog/questlog_lang/english.lang"
OUT = ROOT / "data/quests_flags.json"

def load_lang(path: Path) -> dict[int, str]:
    texts = {}
    pat = re.compile(r'^"(\d+)":\s*"(.*)"\s*$')
    for line in path.read_text(encoding="utf-8").splitlines():
        m = pat.match(line.strip())
        if m:
            texts[int(m.group(1))] = m.group(2)
    return texts

def main() -> int:
    src = QUESTS_HPP.read_text(encoding="utf-8")
    texts = load_lang(ENGLISH_LANG)

    # ADD_TALK_LIST_DATA_ARGS(name, index, msgid) and
    # ADD_TALK_LIST_IF_DATA_ARGS(name, index, msgid, <condition...>);
    # Conditions may span lines (nested esd_or / esd_and).
    entry_re = re.compile(
        r"ADD_TALK_LIST(_IF)?_DATA_ARGS\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*"
        r"(?:,\s*(.*?))?\)\s*;",
        re.DOTALL,
    )
    flag_re = re.compile(r"esd_get_flag\((\d+)\)")

    quests: dict[int, dict] = {}
    for m in entry_re.finditer(src):
        _if, name, _idx, msgid_s, cond = m.groups()
        msgid = int(msgid_s)
        if not (87000000 <= msgid < 88000000):
            continue  # Leave / separator / non-quest entries
        qq = (msgid // 10000) % 100
        ss = (msgid // 100) % 100
        if qq == 0:
            continue  # 870000xx / 870001xx headers
        flags = [int(f) for f in flag_re.findall(cond or "")]
        cond_clean = re.sub(r"\s+", " ", cond).strip() if cond else None
        q = quests.setdefault(qq, {
            "quest_ordinal": qq,
            "name": texts.get(qq * 10000 + 87000000, f"quest_{qq}"),
            "visible_flags": [],
            "visible_condition": None,
            "steps": [],
        })
        if ss == 0:  # the quest's own menu entry: its gate == "quest started"
            q["visible_flags"] = flags
            q["visible_condition"] = cond_clean
        else:
            q["steps"].append({
                "step_ordinal": ss,
                "title": texts.get(msgid, name),
                "completed_text": texts.get(msgid + 1, ""),
                "flags": flags,
                "condition": cond_clean,
                "is_quest_end": ss == 99,
            })

    out = sorted(quests.values(), key=lambda q: q["quest_ordinal"])
    for q in out:
        q["steps"].sort(key=lambda s: s["step_ordinal"])

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")

    n_steps = sum(len(q["steps"]) for q in out)
    n_flagged = sum(1 for q in out for s in q["steps"] if s["flags"])
    print(f"quests: {len(out)}  steps: {n_steps}  steps with flags: {n_flagged}")
    print(f"wrote {OUT}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
