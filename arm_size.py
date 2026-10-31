#!/usr/bin/env python3
# Compila (de verdad, a objetos) el codigo del motor para ARM946E-S en modo Thumb
# con -Os y suma el tamano. Uso: python3 arm_size.py [limite_de_archivos]
import collections, concurrent.futures, json, os, re, shlex, shutil, subprocess, sys

CC = os.environ.get("ARM_CXX", "arm-none-eabi-g++")
SIZE = os.environ.get("ARM_SIZE", "arm-none-eabi-size")
FLAGS = shlex.split(os.environ.get("ARM_FLAGS", "-mcpu=arm946e-s -mthumb -fno-short-enums"))
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
OBJDIR = "/tmp/arm_obj"

shutil.rmtree(OBJDIR, ignore_errors=True)
os.makedirs(OBJDIR)

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

def rewrite(tokens, obj):
    out = [CC]
    i = 1
    while i < len(tokens):
        t = tokens[i]
        if t in DROP_WITH_ARG:
            i += 2
            continue
        if (t in DROP or t.startswith("-flto") or t.startswith("-march=")
                or t.startswith("-mtune=") or t.startswith("-fuse-linker-plugin")
                or t.startswith("-O") or t.startswith("-g")):
            i += 1
            continue
        out.append(t)
        i += 1
    return out + ["-Os", "-c", "-o", obj] + FLAGS

def build(item):
    n, e = item
    obj = os.path.join(OBJDIR, "%04d.o" % n)
    toks = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
    r = subprocess.run(rewrite(toks, obj), cwd=e["directory"],
                       capture_output=True, text=True)
    return e["file"], obj, r.returncode, r.stderr

built = []
fails = []
msgs = collections.Counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
    for f, obj, rc, err in ex.map(build, list(enumerate(sample))):
        if rc == 0:
            built.append((f, obj))
        else:
            fails.append(os.path.relpath(f))
            for line in err.splitlines():
                if "error:" in line:
                    msgs[re.sub(r"^.*error: ", "", line).strip()[:110]] += 1

print(f"\n{len(built)} compilados, {len(fails)} con errores, de {len(sample)} archivos")
if msgs:
    print("\nErrores mas frecuentes:")
    for m, n in msgs.most_common(10):
        print(f"  {n:4d}  {m}")
    print("\nArchivos con errores:")
    for f in fails[:20]:
        print("  ", f)

rows = []
for f, obj in built:
    out = subprocess.run([SIZE, obj], capture_output=True, text=True).stdout.splitlines()
    if len(out) >= 2:
        p = out[1].split()
        rows.append((int(p[0]), int(p[1]), int(p[2]), os.path.relpath(f)))

t = sum(r[0] for r in rows)
d = sum(r[1] for r in rows)
b = sum(r[2] for r in rows)
print(f"\nSuma de los objetos: text {t/1024:.0f} KiB ({t/1048576:.2f} MiB), data {d/1024:.0f} KiB, bss {b/1024:.0f} KiB")
print("(cuenta varias veces el codigo de plantillas repetido; el enlazado lo reduce)")
print("\nLos 12 objetos con mas codigo:")
for text, data, bss, f in sorted(rows, reverse=True)[:12]:
    print(f"  {text/1024:7.0f} KiB  {f}")
