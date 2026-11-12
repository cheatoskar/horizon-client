"""Packages the build into release zips and a manifest for TM Horizon.

Run after build.cmd (build.cmd package does both). Reads the build output (dist\\legacy, build\\plugins) and
packaging\\; writes:

  dist\\tmloader\\Horizon Host\\<version>\\          TMLoader product folder (Forever)
  dist\\release\\Horizon-Client-<version>-<game>.zip  one zip per legacy game (ESWC, Sunrise, Original):
                                                    README.txt, Horizon-Install.cmd, Horizon-Uninstall.cmd,
                                                    horizon\\ (loader\\dinput8.dll + install.ps1, host.dll,
                                                    plugins\\, scripts\\), LICENSE.txt, THIRD-PARTY.txt
  dist\\release\\Horizon-Client-<version>-TMLoader.zip the TMLoader product ("Horizon Host\\" folder; the version
                                                    folder also has LICENSE.txt and THIRD-PARTY.txt)
  dist\\release\\SHA256SUMS.txt                       sha256 of those four zips
  dist\\release\\standalone\\<Name>-<version>-standalone.zip  one zip per dist\\standalone\\<id>\\ (a module that
                                                    also runs without Horizon, e.g. controller-hotplug)
  dist\\manifest.json                               version, every file with size + sha256, every zip

Only the modules in SHIP go into the user packages, each into the games its plugin.json lists. Not shipped:
the sample plugin, porting-kit examples, other people's mods. Extra plugins (e.g. the Legacy Randomizer, built
in its own repo) in extra-plugins\\<id>\\ are added only with --with-extra (test builds).

The zips are reproducible: entries sorted, fixed timestamp, text files with CRLF.
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
STANDALONE = os.path.join(DIST, "standalone")
STANDALONE_NAMES = {"controller-hotplug": "Controller-Hotplug"}
# the modules players get; anything else in the build folders stays out of the zips
SHIP = ["eswc-autosave", "controller-hotplug", "game-settings", "input-display", "wr-hunter", "tmx-browser", "tmx-upload",
        "performance", "scripts", "node-explorer"]
# where the build leaves a plugin folder (DLL, plugin.json, icons), first match wins
PLUGIN_BASES = [os.path.join(LEGACY, "horizon", "plugins"), os.path.join(ROOT, "build", "plugins")]
SCRIPT_EXAMPLES = os.path.join(ROOT, "plugins", "scripts", "examples")
LEGACY_GAMES = (("ESWC", "eswc"), ("Sunrise", "sunrise"), ("Original", "original"))
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def host_version():
    text = open(os.path.join(ROOT, "src", "host", "host.h"), encoding="utf-8").read()
    return re.search(r'kHostVersion = "([^"]+)"', text).group(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_meta(folder):
    return json.load(open(os.path.join(folder, "plugin.json"), encoding="utf-8-sig"))


def plugin_files(folder):
    """The files of a built plugin folder that ship: the DLL plugin.json names, plugin.json and the icons."""
    meta = read_meta(folder)
    names = [meta["dll"], "plugin.json", meta.get("icon", "icon.png"), "icon-alt.png"]
    out = []
    for n in names:
        if n and n not in out and os.path.isfile(os.path.join(folder, n)):
            out.append(n)
    if meta["dll"] not in out:
        sys.exit("plugin %s: %s missing in %s" % (meta["id"], meta["dll"], folder))
    return out


def plugin_dirs(with_extra):
    """(id, folder, games) for every plugin to ship."""
    out = []
    for pid in SHIP:
        folder = next((os.path.join(b, pid) for b in PLUGIN_BASES
                       if os.path.isfile(os.path.join(b, pid, "plugin.json"))), None)
        if not folder:
            sys.exit("plugin %s not built (looked in dist\\legacy\\horizon\\plugins and build\\plugins)" % pid)
        meta = read_meta(folder)
        if meta["id"] != pid:
            sys.exit("plugin folder %s has id %s" % (folder, meta["id"]))
        out.append((pid, folder, [g.lower() for g in meta.get("games", [])]))
    if with_extra and os.path.isdir(EXTRA):
        for pid in sorted(os.listdir(EXTRA)):
            folder = os.path.join(EXTRA, pid)
            if os.path.isfile(os.path.join(folder, "plugin.json")):
                out.append((pid, folder, [g.lower() for g in read_meta(folder).get("games", [])]))
    return out


def copy_plugin(folder, dst):
    os.makedirs(dst, exist_ok=True)
    for n in plugin_files(folder):
        shutil.copy2(os.path.join(folder, n), os.path.join(dst, n))


def copy_tree(src, dst):
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.isdir(s):
            copy_tree(s, d)
        else:
            shutil.copy2(s, d)


def write_text(path, text):
    """Text for players: CRLF, UTF-8 without BOM."""
    text = text.replace("\r\n", "\n").replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def fill(name, version):
    return open(os.path.join(PACK, name), encoding="utf-8").read().replace("@VERSION@", version)


def stage_common(stage, version):
    write_text(os.path.join(stage, "LICENSE.txt"), open(os.path.join(ROOT, "LICENSE"), encoding="utf-8").read())
    write_text(os.path.join(stage, "THIRD-PARTY.txt"), fill("THIRD-PARTY.txt", version))


def stage_legacy(stage, game_key, version, plugins):
    """Legacy Loader layout: dinput8.dll lives in horizon\\loader\\ and Horizon-Install.cmd puts it in place
    (an existing dinput8.dll of another mod becomes dinput8.chain.dll). Unzipping never overwrites a
    foreign dinput8.dll."""
    hz = os.path.join(stage, "horizon")
    os.makedirs(os.path.join(hz, "loader"), exist_ok=True)
    shutil.copy2(os.path.join(LEGACY, "dinput8.dll"), os.path.join(hz, "loader", "dinput8.dll"))
    shutil.copy2(os.path.join(PACK, "install.ps1"), os.path.join(hz, "loader", "install.ps1"))
    shutil.copy2(os.path.join(LEGACY, "horizon", "host.dll"), os.path.join(hz, "host.dll"))
    shipped = []
    for pid, folder, games in plugins:
        if game_key in games:
            copy_plugin(folder, os.path.join(hz, "plugins", pid))
            shipped.append(pid)
    if "scripts" in shipped:
        copy_tree(SCRIPT_EXAMPLES, os.path.join(hz, "scripts"))
    for name in ("Horizon-Install.cmd", "Horizon-Uninstall.cmd"):
        shutil.copy2(os.path.join(PACK, name), os.path.join(stage, name))
    write_text(os.path.join(stage, "README.txt"), fill("README.txt", version))
    stage_common(stage, version)
    return shipped


def stage_tmloader(stage, version, plugins):
    prod = os.path.join(stage, "Horizon Host")
    # TMLoader's version folder has no pre-release part: 0.3.0-rc.1 -> 0.3.0
    ver = os.path.join(prod, version.split("-")[0])
    os.makedirs(ver, exist_ok=True)
    shutil.copy2(os.path.join(PACK, "tmloader", "description.yaml"), os.path.join(prod, "description.yaml"))
    text = fill(os.path.join("tmloader", "version.yaml"), version)
    open(os.path.join(ver, "description.yaml"), "w", encoding="utf-8", newline="\n").write(text)
    shutil.copy2(os.path.join(LEGACY, "horizon", "host.dll"), os.path.join(ver, "HorizonHost.dll"))
    stage_common(ver, version)
    shipped = []
    for pid, folder, games in plugins:
        if "tmnf" in games or "tmuf" in games:
            copy_plugin(folder, os.path.join(ver, "plugins", pid))
            shipped.append(pid)
    return prod, shipped


def files_of(base):
    out = []
    for dirpath, dirs, names in os.walk(base):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(dirpath, n)
            out.append({"path": os.path.relpath(p, base).replace("\\", "/"), "size": os.path.getsize(p), "sha256": sha256(p)})
    return out


def zip_dir(src, zip_path):
    entries = []
    for dirpath, _, names in os.walk(src):
        for n in names:
            p = os.path.join(dirpath, n)
            entries.append((os.path.relpath(p, src).replace("\\", "/"), p))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, p in sorted(entries):
            info = zipfile.ZipInfo(arc, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(p, "rb") as f:
                z.writestr(info, f.read())


def standalone_packages():
    """(id, name, version, folder) for every dist\\standalone\\<id>\\. The version is the module's own
    (dist\\legacy\\horizon\\plugins\\<id>\\plugin.json)."""
    out = []
    if not os.path.isdir(STANDALONE):
        return out
    for pid in sorted(os.listdir(STANDALONE)):
        folder = os.path.join(STANDALONE, pid)
        if not os.path.isdir(folder):
            continue
        manifest = os.path.join(LEGACY, "horizon", "plugins", pid, "plugin.json")
        if not os.path.isfile(manifest):
            sys.exit("standalone %s: missing %s" % (pid, manifest))
        meta = json.load(open(manifest, encoding="utf-8-sig"))
        out.append((pid, STANDALONE_NAMES.get(pid, pid), meta["version"], folder))
    return out


def main():
    with_extra = "--with-extra" in sys.argv[1:]
    version = host_version()
    for need in (os.path.join(LEGACY, "dinput8.dll"), os.path.join(LEGACY, "horizon", "host.dll")):
        if not os.path.isfile(need):
            sys.exit("missing %s: run build.cmd first" % need)
    plugins = plugin_dirs(with_extra)
    staging = os.path.join(DIST, "staging")
    release = os.path.join(DIST, "release")
    for d in (staging, release, os.path.join(DIST, "tmloader")):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    packages = []  # (name in the zip file name, manifest game key, stage folder, shipped plugin ids)
    for game, key in LEGACY_GAMES:
        stage = os.path.join(staging, game)
        packages.append((game, key, stage, stage_legacy(stage, key, version, plugins)))
    stage = os.path.join(staging, "TMLoader")
    prod, shipped = stage_tmloader(stage, version, plugins)
    packages.append(("TMLoader", "forever", stage, shipped))
    # the TMLoader product folder on its own (for TM Horizon or a manual copy into TMLoader's database)
    copy_tree(stage, os.path.join(DIST, "tmloader"))

    manifest = {"name": "Horizon Client", "folder_name": "Horizon Host", "version": version,
                "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "plugin_abi": 1, "packages": []}
    sums = []
    for name, key, stage, shipped in packages:
        zip_path = os.path.join(release, "Horizon-Client-%s-%s.zip" % (version, name))
        zip_dir(stage, zip_path)
        digest = sha256(zip_path)
        sums.append("%s *%s" % (digest, os.path.basename(zip_path)))
        manifest["packages"].append({"game": key, "kind": "tmloader" if name == "TMLoader" else "legacy",
                                     "zip": os.path.relpath(zip_path, DIST).replace("\\", "/"),
                                     "size": os.path.getsize(zip_path), "sha256": digest, "plugins": shipped,
                                     "files": files_of(stage)})
    with open(os.path.join(release, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sums) + "\n")
    manifest["standalone"] = []
    alone = os.path.join(release, "standalone")
    for pid, name, ver, folder in standalone_packages():
        os.makedirs(alone, exist_ok=True)
        zip_path = os.path.join(alone, "%s-%s-standalone.zip" % (name, ver))
        zip_dir(folder, zip_path)
        manifest["standalone"].append({"id": pid, "version": ver, "zip": os.path.relpath(zip_path, DIST).replace("\\", "/"),
                                       "size": os.path.getsize(zip_path), "sha256": sha256(zip_path), "files": files_of(folder)})
    manifest["tmloader_product"] = {"folder": "tmloader/Horizon Host", "files": files_of(os.path.join(DIST, "tmloader", "Horizon Host"))}
    with open(os.path.join(DIST, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
    shutil.rmtree(staging, ignore_errors=True)
    for p in manifest["packages"]:
        print("%-8s %s  %s  (%s)" % (p["game"], p["sha256"], p["zip"], ", ".join(p["plugins"])))
    for p in manifest["standalone"]:
        print("%-8s %s  %s" % ("alone", p["sha256"], p["zip"]))
    print("sums:     dist\\release\\SHA256SUMS.txt")
    print("manifest: dist\\manifest.json")


if __name__ == "__main__":
    main()
