import json, sys, urllib.request, os
repo, tag, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
tok = json.load(urllib.request.urlopen(f"https://auth.docker.io/token?service=registry.docker.io&scope=repository:{repo}:pull"))["token"]
def get(path, accept):
    r = urllib.request.Request(f"https://registry-1.docker.io/v2/{repo}/{path}", headers={"Authorization": "Bearer "+tok, "Accept": accept})
    return urllib.request.urlopen(r)
acc = "application/vnd.oci.image.index.v1+json,application/vnd.docker.distribution.manifest.list.v2+json,application/vnd.docker.distribution.manifest.v2+json,application/vnd.oci.image.manifest.v1+json"
m = json.load(get(f"manifests/{tag}", acc))
if "manifests" in m:
    d = [x for x in m["manifests"] if x.get("platform",{}).get("architecture")=="amd64"][0]["digest"]
    m = json.load(get(f"manifests/{d}", acc))
for i, l in enumerate(m["layers"]):
    print(i, l["digest"], l["size"], flush=True)
    with get(f"blobs/{l['digest']}", "*/*") as r, open(f"{out}/layer{i:02d}.tgz", "wb") as f:
        while True:
            b = r.read(1<<20)
            if not b: break
            f.write(b)
