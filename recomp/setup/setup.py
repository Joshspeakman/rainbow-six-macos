#!/usr/bin/env python3
"""setup.py: recompile the player's own game on their Mac, using only what the app carries.

Run by the app (host/setup_ui.m) with its bundled Python. Prints one JSON object per line: progress for the setup window.

    setup.py --game <folder holding the game, or "" if it is already in place> --root <~/Games/Name>
             --setup <App/Contents/Resources/setup> --tools <unpacked compiler and Python>

Everything is written under <root>; the game folder that was dropped is only read. An interrupted setup picks up from the
last finished file. A store's copy may carry the store's wrapper on its executable (Steam's SteamStub): game.json names,
per accepted executable, the step that takes it off the copy here, from the player's own files. Nothing else touches copy
protection.
"""
import argparse, concurrent.futures, hashlib, json, os, shutil, signal, subprocess, sys, threading, time


def say(**msg):
    print(json.dumps(msg), flush=True)


class Stop(Exception):
    """A failure with a sentence the player can act on."""


# how much of the bar each stage takes: set from timing a real run
STAGES = [("copy", 4), ("analyse", 6), ("lift", 22), ("compile", 62), ("link", 3), ("art", 3)]


class Bar:
    def __init__(self):
        self.done = 0

    def stage(self, name, text):
        self.name = name
        self.span = dict(STAGES)[name]
        self.text = text
        self.at(0)

    def at(self, fraction):
        say(stage=self.name, pct=round(self.done + self.span * min(1.0, fraction), 1), text=self.text)

    def finish(self):
        self.done += self.span


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_ci(folder, relative):
    """The file `relative` under `folder`, matching names case-insensitively as Windows would."""
    here = folder
    for part in relative.split("/"):
        try:
            names = {n.lower(): n for n in os.listdir(here)}
        except OSError:
            return None
        if part.lower() not in names:
            return None
        here = os.path.join(here, names[part.lower()])
    return here


def locate_game(folder, cfg):
    """The folder under `folder` (up to 3 levels down) that holds the game's executable."""
    queue = [(folder, 0)]
    while queue:
        here, depth = queue.pop(0)
        if find_ci(here, cfg["exe"]):
            return here
        if depth < 3:
            try:
                names = sorted(os.listdir(here))
            except OSError:
                continue
            queue += [(os.path.join(here, n), depth + 1) for n in names if os.path.isdir(os.path.join(here, n))]
    return None


def check_game(game, cfg):
    exe = find_ci(game, cfg["exe"])
    if not exe:
        raise Stop(f"That folder doesn't have {cfg['exe']} in it.")
    if sha256(exe) not in cfg["exe_sha256"]:
        raise Stop(f"This isn't the version of {cfg['name']} this app works with. It needs {cfg['version']}.")
    missing = [r for r in cfg["required"] if not find_ci(game, r)]
    if missing:
        raise Stop(f"The game folder is incomplete: {missing[0]} is missing.")


def copy_game(src, dest, bar):
    """dest becomes a copy of src: an APFS clone where the disk allows, so it takes almost no extra space."""
    part = dest + ".part"
    shutil.rmtree(part, ignore_errors=True)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if subprocess.run(["/bin/cp", "-cR", src, part], capture_output=True).returncode != 0:
        shutil.rmtree(part, ignore_errors=True)
        r = subprocess.run(["/bin/cp", "-R", src, part], capture_output=True, text=True)
        if r.returncode != 0:
            shutil.rmtree(part, ignore_errors=True)
            raise Stop("Couldn't copy the game: is there enough free space? " + r.stderr.strip()[:160])
    shutil.rmtree(dest, ignore_errors=True)
    os.rename(part, dest)
    bar.at(1)


def merge_addons(src, dest, cfg):
    """game.json "addons": pieces the game folder may lack that sit beside it instead (Ghost Recon's expansions, downloaded
    on a Mac as separate Steam depots): each one found is copied in. Missing ones are fine; the launch window says how."""
    for add in cfg.get("addons", []):
        if find_ci(dest, add["into"]):
            continue
        for rel in add.get("look", []):
            there = os.path.normpath(os.path.join(src, rel))
            if os.path.isdir(there):
                into = os.path.join(dest, add["into"])
                os.makedirs(os.path.dirname(into), exist_ok=True)
                if subprocess.run(["/bin/cp", "-cR", there, into], capture_output=True).returncode != 0:
                    subprocess.run(["/bin/cp", "-R", there, into], capture_output=True)
                break


def patches(cfg, original):
    """The steps run on the copied executable: game.json's exe_patch, then the accepted version's own ("patch")."""
    v = cfg["exe_sha256"].get(sha256(original))
    return list(cfg.get("exe_patch", [])) + (list(v.get("patch", [])) if isinstance(v, dict) else [])


def folder_gb(path):
    total = 0
    for here, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(here, f)).st_size
            except OSError:
                pass
    return total / (1 << 30)


def tag_of(name, cfg):
    """A DLL's tag (its generated files' and functions' prefix): the file name with every character a C name can't hold
    as "_" (zlib-1.1.3.dll -> zlib_1_1_3_dll)."""
    tag = "".join(ch if ch.isalnum() else "_" for ch in os.path.basename(name))
    return tag.lower() if cfg.get("tag_lowercase") else tag


class Run:
    def __init__(self, a):
        self.setup = a.setup
        self.root = a.root
        self.cfg = json.load(open(os.path.join(a.setup, "game.json")))
        self.work = os.path.join(a.root, ".setup", "work")
        self.out = os.path.join(self.work, "out")
        self.py = sys.executable
        self.llvm = os.path.join(a.tools, "llvm", "bin")
        self.toolsrc = os.path.join(a.setup, "tools")
        self.env = dict(os.environ, PYTHONPATH=self.toolsrc)

    def tool(self, script, *args):
        r = subprocess.run([self.py, os.path.join(self.toolsrc, script), *map(str, args)], env=self.env, capture_output=True, text=True)
        if r.returncode != 0:
            raise Stop(f"Setup failed in {script}: " + (r.stderr or r.stdout).strip()[-300:])
        return r.stdout

    def patch(self, step, exe):
        """One step on the copied executable: "script.py" (given the file), or ["script.py", "{exe}", ...]."""
        if isinstance(step, str):
            self.tool(step, exe)
        else:
            self.tool(step[0], *[a.replace("{exe}", exe) for a in step[1:]])

    def analysis(self, path, out):
        """The shipped list of function addresses for exactly this file, or the slower analysis if there is none."""
        ship = os.path.join(self.setup, "lists", sha256(path) + ".json")
        if os.path.exists(ship):
            shutil.copyfile(ship, out)
        else:
            self.tool("analyze.py", path, out)

    def items(self, game, exe):
        """What gets analysed and lifted: the (patched) executable and the game's own DLLs, some moved to a free address."""
        cfg = self.cfg
        items = [{"path": exe, "main": True}]
        for lib in cfg.get("libs", []):
            src = find_ci(game, lib["file"])
            if not src:
                raise Stop(f"The game folder is incomplete: {lib['file']} is missing.")
            if lib.get("rebase"):
                moved = os.path.join(self.work, os.path.basename(lib["file"]))
                self.tool("rebase_pe.py", src, moved, lib["rebase"])
                src = moved
            items.append({"path": src, "file": lib["file"], "tag": tag_of(lib["file"], cfg)})
        return items

    def make_lists(self, game, dest):
        """Release builds only: the address lists setup looks up, keyed by the exact file they describe."""
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.out)
        os.makedirs(dest, exist_ok=True)
        exe = os.path.join(self.work, os.path.basename(self.cfg["exe"]))
        shutil.copyfile(find_ci(game, self.cfg["exe"]), exe)
        os.chmod(exe, 0o644)
        for step in patches(self.cfg, find_ci(game, self.cfg["exe"])):
            self.patch(step, exe)
        for it in self.items(game, exe):
            out = os.path.join(dest, sha256(it["path"]) + ".json")
            self.tool("analyze.py", it["path"], out)
            d = json.load(open(out))
            with open(out, "w") as f:
                json.dump(d, f, separators=(",", ":"))
            say(stage="list", text=os.path.basename(it["path"]))
        shutil.rmtree(self.work, ignore_errors=True)

    def lift_all(self, game, bar):
        cfg, out = self.cfg, self.out
        stamp = os.path.join(self.work, "lifted.ok")
        if os.path.exists(stamp):
            bar.stage("analyse", "Reading the game's code")
            bar.finish()
            bar.stage("lift", "Translating the game's code")
            bar.finish()
            return
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(out)
        exe = os.path.join(self.work, os.path.basename(cfg["exe"]))
        shutil.copyfile(find_ci(game, cfg["exe"]), exe)
        os.chmod(exe, 0o644)
        for step in patches(cfg, find_ci(game, cfg["exe"])):
            self.patch(step, exe)
        bar.stage("analyse", "Reading the game's code")
        items = self.items(game, exe)
        for n, it in enumerate(items):
            it["json"] = os.path.join(self.work, ("analysis" if it.get("main") else it["tag"]) + ".json")
            self.analysis(it["path"], it["json"])
            bar.at((n + 1) / len(items))
        bar.finish()
        bar.stage("lift", "Translating the game's code")
        self.tool("lift.py", exe, items[0]["json"], out, *cfg.get("lift_args", []))
        bar.at(0.7)
        imgs = ["="]
        nimp = len(json.load(open(os.path.join(out, "image.json")))["imports"])
        for it in items[1:]:
            self.tool("lift.py", it["path"], it["json"], out, "--tag", it["tag"], "--imp-base", nimp)
            nimp += len(json.load(open(os.path.join(out, it["tag"] + "_image.json")))["imports"])
            imgs.append(f"{it['tag']}={it['file']}")
        self.tool("link_tables.py", out, *imgs)
        open(stamp, "w").close()
        bar.finish()

    def compile_one(self, src):
        obj = os.path.join(self.objdir, os.path.basename(src)[:-2] + ".o")
        if os.path.exists(obj) and os.path.getsize(obj) > 0:
            return
        cmd = [os.path.join(self.llvm, "clang"), "--target=arm64-apple-macos13", "-nostdlibinc",
               "-isystem", os.path.join(self.setup, "include"), "-c", "-O2", "-ffp-contract=off", "-fno-strict-aliasing",
               *self.cfg.get("cc_args", []), "-I", os.path.join(self.setup, "runtime"), "-I", self.out, "-o", obj + ".tmp", src]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Stop("Setup failed compiling the game: " + r.stderr.strip()[-300:])
        os.replace(obj + ".tmp", obj)

    def compile_all(self, bar):
        bar.stage("compile", "Building the game for your Mac")
        self.objdir = os.path.join(self.work, "obj")
        os.makedirs(self.objdir, exist_ok=True)
        names = sorted(n for n in os.listdir(self.out) if n.endswith(".c") and ("lift_" in n or n == "fntab.c"))
        files = [os.path.join(self.out, n) for n in names] + [os.path.join(self.setup, "game_tls.c")]
        ram_gb = int(subprocess.run(["/usr/sbin/sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout) / (1 << 30)
        jobs = max(1, min(os.cpu_count() or 2, int(ram_gb / 2)))
        finished, lock = [0], threading.Lock()

        def one(src):
            self.compile_one(src)
            with lock:
                finished[0] += 1
                bar.at(finished[0] / len(files))

        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            for f in [pool.submit(one, s) for s in files]:
                f.result()
        bar.finish()

    def link(self, game, bar):
        bar.stage("link", "Putting it together")
        out = os.path.join(self.root, "recomp")
        os.makedirs(out, exist_ok=True)
        objs = sorted(os.path.join(self.objdir, n) for n in os.listdir(self.objdir) if n.endswith(".o"))
        lst = os.path.join(self.work, "objs.txt")
        with open(lst, "w") as f:
            f.write("\n".join(objs))
        target = os.path.join(out, "libgame.dylib")
        cmd = [os.path.join(self.llvm, "ld64.lld"), "-dylib", "-arch", "arm64", "-platform_version", "macos", "13.0", "13.0",
               "-undefined", "dynamic_lookup", "-install_name", "@rpath/libgame.dylib", "-o", target + ".tmp", "-filelist", lst]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Stop("Setup failed linking the game: " + r.stderr.strip()[-300:])
        os.replace(target + ".tmp", target)
        shutil.copyfile(os.path.join(self.work, os.path.basename(self.cfg["exe"])), os.path.join(out, os.path.basename(self.cfg["exe"])))
        bar.finish()

    def art(self, game, bar):
        """The launch window's art, read from the player's own files. A plain title is fine if this can't be done."""
        bar.stage("art", "Finishing up")
        step, helper = self.cfg.get("art"), self.cfg.get("art_helper")
        if step:
            try:
                if helper:      # the script gathers the pictures, the app's compiled helper (bin/) composes them
                    src = os.path.join(self.work, "art-src")
                    self.tool(step, game, src)
                    exe = os.path.join(self.setup, "bin", os.path.splitext(os.path.basename(helper))[0])
                    subprocess.run([exe, src, os.path.join(self.root, "art")], capture_output=True, timeout=120)
                elif isinstance(step, list):    # ["script.py", "{art}", ...], or a list of those: {game} and {art} filled in
                    for one in (step if isinstance(step[0], list) else [step]):
                        try:
                            self.tool(one[0], *[a.replace("{game}", game).replace("{art}", os.path.join(self.root, "art")) for a in one[1:]])
                        except (Stop, OSError, subprocess.SubprocessError):
                            pass                    # one picture missing leaves the others
                else:
                    self.tool(step, game, os.path.join(self.root, "art"))
            except (Stop, OSError, subprocess.SubprocessError):
                pass
        url = self.cfg.get("art_url")
        banner = os.path.join(self.root, "art", "banner.jpg")
        if url and not os.path.exists(banner):
            os.makedirs(os.path.dirname(banner), exist_ok=True)
            subprocess.run(["/usr/bin/curl", "-fsSL", "-m", "30", "-o", banner + ".part", url], capture_output=True)
            if os.path.exists(banner + ".part"):
                os.replace(banner + ".part", banner)
        bar.finish()


def main():
    try:
        os.setsid()                             # the app can stop this and everything it started in one signal
    except OSError:
        pass                                    # already leading its own group when the app starts it
    signal.signal(signal.SIGTERM, lambda *_: os._exit(143))
    ap = argparse.ArgumentParser()
    for name in ("game", "root", "setup", "tools"):
        ap.add_argument("--" + name, required=True)
    ap.add_argument("--make-lists")
    a = ap.parse_args()
    run = Run(a)
    if a.make_lists:
        run.make_lists(a.game, a.make_lists)
        return
    cfg = run.cfg
    bar = Bar()
    dest = os.path.join(a.root, "game-files")
    os.makedirs(a.root, exist_ok=True)
    bar.stage("copy", "Bringing in the game files")
    src = None
    if a.game:
        src = locate_game(a.game, cfg)
        if not src:
            raise Stop(f"That folder doesn't have {cfg['exe']} in it.")
        check_game(src, cfg)
    # the space setup takes: the build's, plus the game's when it comes from another disk (on the same disk the copy is an
    # APFS clone, which takes almost none). game.json's space_gb is the total for a copy from elsewhere.
    free_gb = shutil.disk_usage(a.root).free / (1 << 30)
    need = cfg.get("space_gb", 4)
    if not src or os.path.realpath(src) == os.path.realpath(dest):
        need = cfg.get("build_gb", 2)                                       # already in place
    elif os.stat(src).st_dev == os.stat(a.root).st_dev:
        need = cfg.get("build_gb", max(2, need - folder_gb(src)))           # cloned
    if free_gb < need:
        raise Stop(f"Setup needs about {need:.0f} GB free on your Mac. There's {free_gb:.1f} GB.")
    if src:
        if os.path.realpath(src) != os.path.realpath(dest):
            copy_game(src, dest, bar)
            merge_addons(src, dest, cfg)
    else:
        check_game(dest, cfg)
    bar.finish()
    run.lift_all(dest, bar)
    run.compile_all(bar)
    run.link(dest, bar)
    run.art(dest, bar)
    ready = {"code": cfg["code"], "exe": sha256(find_ci(dest, cfg["exe"])), "when": int(time.time())}
    with open(os.path.join(a.root, "recomp", "ready.json"), "w") as f:
        json.dump(ready, f)
    shutil.rmtree(os.path.join(a.root, ".setup"), ignore_errors=True)
    say(stage="done", pct=100, text="Ready")


if __name__ == "__main__":
    try:
        main()
    except Stop as e:
        say(stage="error", text=str(e))
        sys.exit(1)
    except Exception as e:
        say(stage="error", text=f"Setup stopped unexpectedly: {e}")
        raise
