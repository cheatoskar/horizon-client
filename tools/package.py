"""Packages the build into release folders, zips and a manifest for TM Horizon.

Run after build.cmd (build.cmd package does both). Reads only dist\\legacy (the build output) and
packaging\\; writes:

  dist\\tmloader\\Horizon Host\\<version>\\        TMLoader product (Forever)
  dist\\release\\Horizon-Host-<version>-<game>.zip  one zip per game (ESWC, Sunrise, Forever)
  dist\\manifest.json                             version, every file with size + sha256, every zip

Extra plugins (e.g. the Legacy Randomizer, built in its own repo) are picked up from
extra-plugins\\<id>\\ (a plugin folder with plugin.json and the DLL); they go into the zips of the games
their plugin.json lists.
"""
import hashlib
import json
import os
import re
import shutil
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
LEGACY = os.path.join(DIST, "legacy")
PACK = os.path.join(ROOT, "packaging")
EXTRA = os.path.join(ROOT, "extra-plugins")


def host_version():
    text = open(os.path.join(ROOT, "src", "host", "host.h"), encoding="utf-8").read()
    return re.search(r'kHostVersion = "([^"]+)"', text).group(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def plugin_dirs():
    """(id, folder, games) for every plugin to ship: dist\\legacy\\horizon\\plugins\\* and extra-plugins\\*."""
    out = []
    for base in (os.path.join(LEGACY, "horizon", "plugins"), EXTRA):
        if not os.path.isdir(base):
            continue
        for pid in sorted(os.listdir(base)):
            folder = os.path.join(base, pid)
            manifest = os.path.join(folder, "plugin.json")
            if not os.path.isfile(manifest):
                continue
            meta = json.load(open(manifest, encoding="utf-8-sig"))
            games = [g.lower() for g in meta.get("games", [])] or ["eswc", "sunrise", "original", "tmnf", "tmuf"]
            out.append((pid, folder, games))
    return out


def copy_tree(src, dst):
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.isdir(s):
            copy_tree(s, d)
        elif not name.lower().endswith((".pdb", ".lib", ".exp", ".ilk")):
            shutil.copy2(s, d)


def stage_legacy(stage, game_keys):
    """Legacy Loader layout: dinput8.dll lives in horizon\\loader\\ and Horizon-Install.cmd puts it in place
    (an existing dinput8.dll becomes dinput8.chain.dll)."""
    os.makedirs(os.path.join(stage, "horizon", "loader"), exist_ok=True)
    shutil.copy2(os.path.join(LEGACY, "dinput8.dll"), os.path.join(stage, "horizon", "loader", "dinput8.dll"))
    shutil.copy2(os.path.join(LEGACY, "horizon", "host.dll"), os.path.join(stage, "horizon", "host.dll"))
    for pid, folder, games in plugin_dirs():
        if any(g in games for g in game_keys):
            copy_tree(folder, os.path.join(stage, "horizon", "plugins", pid))
    for name in ("Horizon-Install.cmd", "Horizon-Uninstall.cmd", "install.ps1", "README-Horizon.txt"):
        src = os.path.join(PACK, name)
        dst = os.path.join(stage, name if name != "install.ps1" else os.path.join("horizon", "loader", "install.ps1"))
        shutil.copy2(src, dst)


def stage_tmloader(stage, version):
    prod = os.path.join(stage, "Horizon Host")
    ver = os.path.join(prod, version)
    os.makedirs(ver, exist_ok=True)
    shutil.copy2(os.path.join(PACK, "tmloader", "description.yaml"), os.path.join(prod, "description.yaml"))
    text = open(os.path.join(PACK, "tmloader", "version.yaml"), encoding="utf-8").read().replace("@VERSION@", version)
    open(os.path.join(ver, "description.yaml"), "w", encoding="utf-8", newline="\n").write(text)
    shutil.copy2(os.path.join(LEGACY, "horizon", "host.dll"), os.path.join(ver, "HorizonHost.dll"))
    for pid, folder, games in plugin_dirs():
        if "tmnf" in games or "tmuf" in games:
            copy_tree(folder, os.path.join(ver, "plugins", pid))
    return prod


def files_of(base):
    out = []
    for dirpath, _, names in os.walk(base):
        for n in sorted(names):
            p = os.path.join(dirpath, n)
            out.append({"path": os.path.relpath(p, base).replace("\\", "/"), "size": os.path.getsize(p), "sha256": sha256(p)})
    return out


def zip_dir(src, zip_path):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, names in os.walk(src):
            for n in sorted(names):
                p = os.path.join(dirpath, n)
                z.write(p, os.path.relpath(p, src))


def main():
    version = host_version()
    for need in (os.path.join(LEGACY, "dinput8.dll"), os.path.join(LEGACY, "horizon", "host.dll")):
        if not os.path.isfile(need):
            sys.exit("missing %s: run build.cmd first" % need)
    staging = os.path.join(DIST, "staging")
    release = os.path.join(DIST, "release")
    for d in (staging, release, os.path.join(DIST, "tmloader")):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    packages = []
    # ESWC and Sunrise: Legacy Loader
    for game, keys in (("ESWC", ["eswc"]), ("Sunrise", ["sunrise"])):
        stage = os.path.join(staging, game)
        stage_legacy(stage, keys)
        packages.append((game, stage))
    # Forever: TMLoader product + the Legacy Loader for players without TMLoader
    stage = os.path.join(staging, "Forever")
    stage_tmloader(os.path.join(stage, "TMLoader"), version)
    stage_legacy(os.path.join(stage, "Without TMLoader"), ["tmnf", "tmuf"])
    shutil.copy2(os.path.join(PACK, "README-Horizon.txt"), os.path.join(stage, "README-Horizon.txt"))
    packages.append(("Forever", stage))
    # the TMLoader product folder on its own (for TM Horizon or a manual copy into TMLoader's database)
    copy_tree(os.path.join(stage, "TMLoader"), os.path.join(DIST, "tmloader"))

    manifest = {"name": "Horizon Host", "version": version, "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "plugin_abi": 1, "packages": []}
    for game, stage in packages:
        zip_path = os.path.join(release, "Horizon-Host-%s-%s.zip" % (version, game))
        zip_dir(stage, zip_path)
        manifest["packages"].append({"game": game.lower(), "zip": os.path.relpath(zip_path, DIST).replace("\\", "/"),
                                     "size": os.path.getsize(zip_path), "sha256": sha256(zip_path), "files": files_of(stage)})
    manifest["tmloader_product"] = {"folder": "tmloader/Horizon Host", "files": files_of(os.path.join(DIST, "tmloader", "Horizon Host"))}
    with open(os.path.join(DIST, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
    shutil.rmtree(staging, ignore_errors=True)
    for p in manifest["packages"]:
        print("%-8s %s  %s" % (p["game"], p["sha256"], p["zip"]))
    print("manifest: dist\\manifest.json")


if __name__ == "__main__":
    main()
