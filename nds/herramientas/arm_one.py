import os, shlex, subprocess, sys
src = open("arm_check.py").read()
exec(src.split("ok = 0")[0], globals())
F = os.environ["F"]
e = next(x for x in db if x["file"].endswith(F))
toks = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
r = subprocess.run(rewrite(toks), cwd=e["directory"], capture_output=True, text=True)
print("codigo de salida:", r.returncode)
print("\n".join(r.stderr.splitlines()[:30]))
