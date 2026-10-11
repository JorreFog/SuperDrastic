import sys
pid = sys.argv[1]
rows = []
cur = None
for l in open(f"/proc/{pid}/smaps"):
    w = l.split()
    if "-" in w[0] and len(w) >= 5 and not w[0].endswith(":"):
        a, b = w[0].split("-"); cur = dict(lo=int(a, 16), hi=int(b, 16), perm=w[1], name=" ".join(w[5:]) if len(w) > 5 else "", rss=0, ahp=0, shp=0)
        rows.append(cur)
    elif w[0] == "Rss:": cur["rss"] = int(w[1])
    elif w[0] == "AnonHugePages:": cur["ahp"] = int(w[1])
    elif w[0] == "ShmemPmdMapped:": cur["shp"] = int(w[1])
for r in rows:
    sz = (r["hi"] - r["lo"]) // 1024
    if sz >= 2048 or r["rss"] >= 1024:
        print("%8d KB rss %7d thp %6d %x-%x %s %s" % (sz, r["rss"], r["ahp"] + r["shp"], r["lo"], r["hi"], r["perm"], r["name"][:56]))
