import re, sys

# Emit squigulator's own R10.4 9-mer means as a <kmer>\t<level_pA> TSV, so the reference
# encoding and the simulated signal come from THE SAME model. Mixing models would make the
# sweep measure model mismatch rather than dwell sensitivity: squigulator gives AAAAAAAAA
# 54.316 pA where Icarust's table gives 57.203, a ~3 pA gap against a ~1.2 pA level stdev.
h = "/root/squigulator/src/model.h"
out = sys.argv[1]
marker = "r10_4_nucleotide_9mer_template_model_builtin_data[] = {"

s = open(h).read()
i = s.index(marker) + len(marker)
j = s.index("};", i)
body = s[i:j]

# Each entry looks like:  54.31598354, 1.194564,  // AAAAAAAAA
# The trailing comma after the stdev is absent on the final entry, so it is optional.
rows = re.findall(r"([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,?\s*//\s*([ACGT]{9})", body)
print("parsed %d entries (expect %d)" % (len(rows), 4 ** 9))
if len(rows) != 4 ** 9:
    raise SystemExit("unexpected entry count; the table layout is not what was assumed")

# Our loader ignores the k-mer string and trusts LINE ORDER to be lexicographic ACGT
# (the 2-bit packing order). Verify that rather than assume it.
code = {"A": 0, "C": 1, "G": 2, "T": 3}
bad = 0
for idx, (_, _, kmer) in enumerate(rows):
    v = 0
    for ch in kmer:
        v = v * 4 + code[ch]
    if v != idx:
        bad += 1
        if bad <= 3:
            print("  ORDER MISMATCH at line %d: %s packs to %d" % (idx, kmer, v))
print("order check: %d mismatches" % bad)
if bad:
    raise SystemExit("table is not in lexicographic ACGT order; loader assumption broken")

with open(out, "w") as f:
    for mean, _, kmer in rows:
        f.write("%s\t%s\n" % (kmer, mean))
print("wrote %s" % out)
