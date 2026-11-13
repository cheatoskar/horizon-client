#!/usr/bin/env python3
"""Turns a Horizon Performance series file (<stamp>_series.json) into a table "option -> effect".

    python tools/bench_report.py <stamp>_series.json [-o report.md]

Per set (measured runs only, warm-up runs are left out): average FPS, median frame time, 1 % low,
0.1 % low, stutters per minute, each with the difference to the baseline set in percent. Hint column:
"Unterschied kleiner als die Streuung" when the difference of the means is smaller than max-min of the
baseline's own runs (for every metric). No third-party packages.
"""
import argparse
import json
import os
import statistics
import sys

# key in the run record, column title, higher is better
METRICS = [
    ("avgFps", "FPS (Mittel)", True),
    ("medianMs", "Median ms", False),
    ("low1Fps", "1 % Low", True),
    ("low01Fps", "0,1 % Low", True),
    ("stuttersPerMin", "Ruckler/min", False),
]
NO_DIFF = "Unterschied kleiner als die Streuung"


def load(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        data = json.load(f)
    if data.get("kind") != "series":
        raise ValueError("keine Serien-Datei (kind != series): " + path)
    return data


def run_values(run):
    """The metrics of one run; stutters per minute from the run's own length."""
    seconds = run.get("seconds") or 0
    v = {k: run.get(k) for k, _, _ in METRICS if k != "stuttersPerMin"}
    v["stuttersPerMin"] = run.get("stutters", 0) * 60.0 / seconds if seconds > 0 else None
    return v


def analyze(data, baseline_label=None):
    """Returns {"sets": [...], "baseline": label, "skipped": [...]}; one entry per set that has measured runs."""
    per_set = {}
    order = []
    for r in data.get("runs", []):
        if r.get("warmup"):
            continue
        label = r["set"]
        if label not in per_set:
            per_set[label] = []
            order.append(label)
        per_set[label].append(run_values(r))
    set_info = {s["label"]: s for s in data.get("sets", [])}
    skipped = [
        {"label": s["label"], "reason": s.get("skipReason", "")}
        for s in data.get("sets", [])
        if s.get("status") == "skipped"
    ]
    if not order:
        return {"sets": [], "baseline": None, "skipped": skipped}
    base_label = baseline_label if baseline_label in per_set else ("baseline" if "baseline" in per_set else order[0])
    base_runs = per_set[base_label]

    def mean(runs, key):
        vals = [x[key] for x in runs if x[key] is not None]
        return statistics.fmean(vals) if vals else None

    def spread(runs, key):
        vals = [x[key] for x in runs if x[key] is not None]
        return (max(vals) - min(vals)) if len(vals) >= 2 else None

    base_mean = {k: mean(base_runs, k) for k, _, _ in METRICS}
    base_spread = {k: spread(base_runs, k) for k, _, _ in METRICS}
    out = []
    for label in order:
        runs = per_set[label]
        row = {"label": label, "runs": len(runs), "is_baseline": label == base_label, "metrics": {}}
        beyond = []
        unknown = False
        for key, title, _ in METRICS:
            m = mean(runs, key)
            b = base_mean[key]
            entry = {"mean": m, "delta": None, "percent": None, "beyond": False}
            if label != base_label and m is not None and b is not None:
                entry["delta"] = m - b
                if b != 0:
                    entry["percent"] = (m - b) / b * 100.0
                sp = base_spread[key]
                if sp is None:
                    unknown = True
                elif abs(m - b) > 0 and abs(m - b) >= sp:
                    entry["beyond"] = True
                    beyond.append(title)
            row["metrics"][key] = entry
        if row["is_baseline"]:
            sp_known = any(v is not None for v in base_spread.values())
            row["hint"] = "Bezug" if sp_known else "Bezug (nur ein Lauf)"
        elif unknown:
            row["hint"] = "Streuung unbekannt (Baseline hat nur einen Lauf)"
        elif not beyond:
            row["hint"] = NO_DIFF
        else:
            row["hint"] = "über der Streuung: " + ", ".join(beyond)
        info = set_info.get(label, {})
        row["options"] = info.get("options", {})
        notes = sorted({r.get("note", "") for r in data.get("runs", []) if r.get("set") == label and r.get("note")})
        row["notes"] = notes
        out.append(row)
    return {"sets": out, "baseline": base_label, "skipped": skipped}


def fmt_cell(entry, key):
    m = entry["mean"]
    if m is None:
        return "-"
    decimals = 2 if key == "medianMs" else 1
    text = "%.*f" % (decimals, m)
    if entry["percent"] is not None:
        text += " (%+.1f %%)" % entry["percent"]
    elif entry["delta"] is not None:
        text += " (%+.*f)" % (decimals, entry["delta"])
    return text


def render(data, result):
    lines = ["# Benchmark-Serie " + str(data.get("stamp", "")), ""]
    facts = [
        ("Spiel", data.get("game")),
        ("GPU", data.get("gpu")),
        ("CPU", data.get("cpu")),
        ("Auflösung", data.get("resolution")),
        ("TMVS", "ja" if data.get("tmvs") else "nein"),
        ("Modus", data.get("mode")),
        ("Replay", data.get("replay") or None),
        ("Karte", data.get("map") or None),
        ("Fenster", "%s s, davon %s s Einschwingen; %s Läufe pro Set, %s Warmup" % (
            data.get("seconds"), data.get("settle"), data.get("runsPerSet"), data.get("warmupPerSet"))),
    ]
    for name, value in facts:
        if value:
            lines.append("- %s: %s" % (name, value))
    if not data.get("completed", True):
        lines.append("- ACHTUNG: Serie nicht zu Ende gelaufen: %s" % (data.get("abortedWhy") or "unbekannt"))
    lines.append("")
    if not result["sets"]:
        lines.append("Keine gemessenen Läufe in der Datei.")
        return "\n".join(lines) + "\n"
    lines.append("Bezug: Set `%s`. Mittel der gemessenen Läufe (Warmup nicht gezählt), Klammer = Unterschied zum Bezug." % result["baseline"])
    lines.append("")
    head = ["Set", "Läufe"] + [t for _, t, _ in METRICS] + ["Hinweis"]
    lines.append("| " + " | ".join(head) + " |")
    lines.append("|" + "|".join(["---"] * len(head)) + "|")
    for row in result["sets"]:
        cells = [row["label"], str(row["runs"])]
        cells += [fmt_cell(row["metrics"][k], k) for k, _, _ in METRICS]
        cells.append(row["hint"])
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("FPS und Low-Werte: höher ist besser. Median ms und Ruckler/min: niedriger ist besser. Ruckler = Frame länger als das Doppelte des Medians.")
    notes = [(r["label"], n) for r in result["sets"] for n in r["notes"]]
    if notes:
        lines.append("")
        lines.append("Anmerkungen:")
        for label, note in notes:
            lines.append("- %s: %s" % (label, note))
    if result["skipped"]:
        lines.append("")
        lines.append("Nicht gemessen:")
        for s in result["skipped"]:
            lines.append("- %s: %s" % (s["label"], s["reason"]))
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Tabelle 'Option -> Wirkung' aus einer Horizon-Performance-Serie")
    ap.add_argument("series", help="<stamp>_series.json")
    ap.add_argument("-o", "--out", help="Markdown-Datei (Standard: neben der Serien-Datei, .md)")
    ap.add_argument("--baseline", help="Label des Bezugs-Sets (Standard: baseline)")
    args = ap.parse_args(argv)
    data = load(args.series)
    text = render(data, analyze(data, args.baseline))
    out = args.out or os.path.splitext(args.series)[0] + ".md"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    print(text)
    print("geschrieben: " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
