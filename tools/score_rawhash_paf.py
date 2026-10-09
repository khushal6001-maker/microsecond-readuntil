import sys

# Score a RawHash PAF against the truth squigulator embeds in the read id:
#   S1_1!chr20_20Mb!6574!9719!-   ->  name!contig!start!end!strand
#
# Reported per file:
#   mapped    fraction of reads given any target (an off-target file's mapped fraction IS
#             the false-positive rate, since those reads come from a shuffled reference
#             that is not in the index)
#   correct   fraction of reads mapped within tolerance of the true locus and strand.
#             Only meaningful for the on-target file; for the shuffled control there is no
#             true locus, which is the point.
TOL = 500

def score(path, want_truth):
    total = mapped = correct = 0
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) < 9:
            continue
        total += 1
        target = f[5]
        if target == "*":
            continue
        mapped += 1
        if not want_truth:
            continue
        parts = f[0].split("!")
        if len(parts) < 5:
            continue
        try:
            t_start, t_end = int(parts[2]), int(parts[3])
        except ValueError:
            continue
        t_strand = parts[4]
        if f[4] != t_strand:
            continue
        p_start, p_end = int(f[7]), int(f[8])
        # squigulator's coordinates are on the forward strand regardless of read strand,
        # so compare intervals rather than just starts.
        if min(abs(p_start - t_start), abs(p_end - t_end)) <= TOL:
            correct += 1
    return total, mapped, correct

on_total, on_mapped, on_correct = score(sys.argv[1], True)
off_total, off_mapped, _ = score(sys.argv[2], False)
pct = lambda a, b: (100.0 * a / b) if b else 0.0
print("%d,%.1f,%.1f,%d,%.1f" % (on_total, pct(on_mapped, on_total),
                                pct(on_correct, on_total), off_total,
                                pct(off_mapped, off_total)))
