#!/usr/bin/env python3
# Comprobacion de sintaxis de DevilutionX con un compilador ARM (no genera codigo).
# Uso: python3 arm_check.py [numero_de_archivos]
import collections, concurrent.futures, json, os, re, shlex, subprocess, sys

CC = os.environ.get("ARM_CXX", "arm-none-eabi-g++")
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 30

db = json.load(open("build-arm/compile_commands.json"))
entries = [e for e in db
           if e["file"].endswith(".cpp")
           and "/Source/" in e["file"]
           and "/_deps/" not in e["file"]]
entries.sort(key=lambda e: e["file"])
step = max(1, len(entries) // LIMIT)
sample = entries[::step][:LIMIT]

DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}
DROP = {"-MD", "-MMD", "-MP", "-c", "-pie", "-fPIE", "-fPIC"}

def rewrite(tokens):
    out = [CC]
    i = 1
    while i < len(tokens):
        t = tokens[i]
        if t in DROP_WITH_ARG:
            i += 2
            continue
        if (t in DROP or t.startswith("-flto") or t.startswith("-march=")
                or t.startswith("-mtune=") or t.startswith("-fuse-linker-plugin")):
            i += 1
            continue
        out.append(t)
        i += 1
    return out + ["-fsyntax-only", "-mcpu=arm946e-s", "-mthumb", "-fno-short-enums", "-DLUA_32BITS"]

def check(e):
    toks = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
    r = subprocess.run(rewrite(toks), cwd=e["directory"],
                       capture_output=True, text=True)
    return e["file"], r.returncode, r.stderr

ok = 0
fails = []
msgs = collections.Counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
    for f, rc, err in ex.map(check, sample):
        if rc == 0:
            ok += 1
            continue
        fails.append(os.path.relpath(f))
        for line in err.splitlines():
            if "error:" in line:
                msgs[re.sub(r"^.*error: ", "", line).strip()[:110]] += 1

print(f"\n{ok} sin errores, {len(fails)} con errores, de {len(sample)} archivos")
print("\nErrores mas frecuentes:")
for m, n in msgs.most_common(15):
    print(f"  {n:4d}  {m}")
print("\nArchivos con errores:")
for f in fails[:20]:
    print("  ", f)
