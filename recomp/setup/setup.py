#!/usr/bin/env python3
"""setup.py: recompile the player's own game on their Mac, using only what the app carries.

Run by the app (host/setup_ui.m) with its bundled Python. Prints one JSON object per line: progress for the setup window.

    setup.py --game <folder holding the game, or "" if it is already in place> --root <~/Games/Name>
             --setup <App/Contents/Resources/setup> --tools <unpacked compiler and Python>

Everything is written under <root>; the game folder that was dropped is only read. An interrupted setup picks up from the
last finished file. Nothing here touches a store wrapper or copy protection: the games set up this way need none removed.
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


def tag_of(name, cfg):
    tag = os.path.basename(name).replace(".", "_")
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
        exe = os.path.join(self.work, self.cfg["exe"])
        shutil.copyfile(find_ci(game, self.cfg["exe"]), exe)
        os.chmod(exe, 0o644)
        for script in self.cfg.get("exe_patch", []):
            self.tool(script, exe)
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
        exe = os.path.join(self.work, cfg["exe"])
        shutil.copyfile(find_ci(game, cfg["exe"]), exe)
        os.chmod(exe, 0o644)
        for script in cfg.get("exe_patch", []):
            self.tool(script, exe)
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
        shutil.copyfile(os.path.join(self.work, self.cfg["exe"]), os.path.join(out, self.cfg["exe"]))
        bar.finish()

    def art(self, game, bar):
        """The launch window's art, read from the player's own files. A plain title is fine if this can't be done."""
        bar.stage("art", "Finishing up")
        step = self.cfg.get("art")
        if step:
            try:
                self.tool(step, game, os.path.join(self.root, "art"))
            except Stop:
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
    free_gb = shutil.disk_usage(a.root).free / (1 << 30)
    if free_gb < cfg.get("space_gb", 4):
        raise Stop(f"Setup needs about {cfg['space_gb']} GB free on your Mac. There's {free_gb:.1f} GB.")
    bar.stage("copy", "Bringing in the game files")
    if a.game:
        src = locate_game(a.game, cfg)
        if not src:
            raise Stop(f"That folder doesn't have {cfg['exe']} in it.")
        check_game(src, cfg)
        if os.path.realpath(src) != os.path.realpath(dest):
            copy_game(src, dest, bar)
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
