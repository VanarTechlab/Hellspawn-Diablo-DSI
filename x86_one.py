import json, os, shlex, subprocess
db = json.load(open("build-arm/compile_commands.json"))
F = os.environ["F"]
e = next(x for x in db if x["file"].endswith(F))
t = shlex.split(e["command"])
out, i = [], 0
while i < len(t):
    if t[i] in ("-o", "-MF", "-MT", "-MQ"):
        i += 2
        continue
    if t[i] in ("-MD", "-MMD", "-MP", "-c"):
        i += 1
        continue
    out.append(t[i])
    i += 1
r = subprocess.run(out + ["-fsyntax-only"], cwd=e["directory"], capture_output=True, text=True)
print("codigo de salida:", r.returncode)
print("\n".join(r.stderr.splitlines()[:15]))
