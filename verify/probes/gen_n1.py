import random
random.seed(7)
n = 256

def mat():
    rows = []
    for i in range(n):
        rows.append("[" + ",".join("%.6f" % random.random() for _ in range(n)) + "]")
    return "[" + ",".join(rows) + "]"

a = mat()
b = mat()
lines = []
lines.append("capability time")
lines.append("def main() -> int:")
lines.append("    a = " + a)
lines.append("    b = " + b)
lines.append("    t0 = now()")
lines.append("    c = a @ b")
lines.append("    s = sum(c)")
lines.append("    t1 = now()")
lines.append("    print(t1 - t0)")
lines.append("    print(c[0][0])")
lines.append("    return 0")
open("probe_097_n1.sg", "w").write("\n".join(lines) + "\n")
print("written", n, "x", n)
