<!--
NOTES FOR THE AUTHORS: delete this block before sending.

Every [BRACKETED CAPITALS] item below is something only you can supply or confirm. Do not
send the letter until each is filled or deleted.

1. Date, dissertation title and its status (submitted, examined, deposited).
2. The read-until manuscript's venue and status (in preparation, under review at X, accepted).
   Bioinformatics asks for related previously published or preprinted work, so if it is
   under review or published, say where.
3. Whether any preprint of either manuscript has been posted.
4. The AI paragraph below now states the use the repository records: every commit carries a
   Claude co-author line, including the pipeline's code, the analysis scripts and the
   dissertation drafts. Correct it if your own use differed. The manuscript's Acknowledgements
   and Supplementary Section S6 say the same and must stay consistent with it.
5. The responsibility sentence in the AI paragraph must be true when you send the letter:
   both authors should have read the whole manuscript and checked its claims.
6. Ritu Garg's approval of the submission.
7. At submission, the journal also requires: an ORCID iD for the submitting author; the CRediT
   roles, entered in the submission form as in the manuscript's Author contributions; and
   copies of the related work disclosed below (the dissertation and the companion manuscript).
8. After approval, cut a new Zenodo release (the manuscript cites v1.0.0, which predates the
   matched-operating-point analysis) and put its DOI into point 3 below and into the manuscript.
-->

Software archive: the manuscript currently cites Zenodo release v1.0.0,
https://doi.org/10.5281/zenodo.23604395 ; see note 8. Delete this line too before sending.

[DATE]

The Editor
*Bioinformatics*, Sequence Analysis section

Dear Editor,

We submit our manuscript "Time-domain noise as an evaluation axis for raw-signal nanopore
mapping" for consideration as an Original Paper in the Sequence Analysis section.

**What the manuscript shows.** Raw-signal mappers segment the nanopore current trace into events
before matching, so their accuracy depends on how event boundaries respond to dwell-time
dispersion, the variation in how long the motor protein holds each k-mer in the pore. Published
evaluations use real datasets, in which that dispersion is fixed by the sequencing run, so no
evaluation measures how sensitive a mapper is to it. We sweep it as a controlled variable in
Squigulator and report two results. First, two segmentation configurations of one pipeline
reverse their order across the sweep, so that an evaluation at constant dwell selects the
configuration that is 10.8-fold worse at the simulator's default dispersion. RawHash2 shows no
such reversal, at its default operating point or at matched false-positive rates, which bounds the
finding to the segmentation choice. Second, the accuracy cost of minimizer sketching in RawHash2
rises from 11.7 to 19.5 percentage points across the range, and is larger at the top of the range
in each of three seeds, also at a matched false-positive rate of 1%, a dependence we have not found
measured elsewhere. We also release a patch that adds dwell variation to Icarust, the read-until
simulator, which otherwise emits constant dwell and so cannot exercise this failure mode.

**Why simulated data.** The journal accepts simulated data alone only in well-justified cases, and
we believe this is one. The variable under study, dwell-time dispersion, is fixed in any real
dataset by the sample, the pore, the chemistry and the run, and cannot be changed while everything
else is held; its effect on a mapper therefore cannot be measured on real data, only on a simulator
that exposes it as a parameter. The claims concern how mappers are evaluated, not how a particular
dataset behaves, and the step from a simulator setting to the dispersion of a real device is not
taken: the manuscript states it as a limitation (Section 5), together with the measurement on
public data that would take it.

**Why *Bioinformatics*.** The contribution concerns how raw-signal mappers are evaluated, and its
recommendations apply directly to tools this journal has published: RawHash, RawHash2, Sigmap,
Sigmoni and Icarust all appeared in *Bioinformatics*. Every number in the manuscript is recomputed
from committed result files by a script released with it, and results that rest on development
runs without captured output are confined to the Supplementary Information, marked, and used in
no claim.

**Related work by the authors, and overlap.** Two pieces of our own work share data with this
manuscript.

1. *M.Tech dissertation.* The manuscript is developed from Chapter 6 of the first author's M.Tech
   dissertation, "[DISSERTATION TITLE]", Department of Computer Engineering, National Institute
   of Technology Kurukshetra, [STATUS AND DATE]. The dissertation is not a peer-reviewed
   publication. Where the two differ, the manuscript supersedes the chapter.

2. *A companion manuscript,* "A Lock-Free Data Plane and the Signal-to-Seed Bottleneck in Nanopore
   Adaptive Sampling" (K. Singh and R. Garg), [VENUE AND STATUS]. It addresses a different
   question: the decision latency of a read-until data plane, and the per-event agreement that
   limits the accuracy of quantised-event matching. The two manuscripts share their simulation
   set-up and part of their data:
   - the same reference (a 20 Mb prefix of chromosome 20), pore model and Squigulator settings;
   - the three-seed RawHash2 result at its default setting (91.8% of reads correctly mapped),
     which appears here as one cell of the dwell sweep (CV 0.44, w = 0) and as context;
   - the per-event agreement and indel fractions in Table 4, which the companion manuscript
     reports as part of its own analysis and which serve here only as supporting evidence for
     the bounding result of Section 3.2;
   - a one-sentence mention of the Icarust patch, which this manuscript describes in full in
     Section 3.4.

   The dwell sweep itself (Tables 2, 3 and 5 and Figures 2 to 4), the ranking reversal, the
   minimizer result, the compatibility conditions of Section 2.3, the matched operating points of
   Section 3.2 and the contents of the Supplementary Information do not appear in the companion
   manuscript. If the companion manuscript is published or
   posted while this one is under review, we will cite it and reduce Table 4 to a citation.

3. *Public draft.* The code, results and a draft of this manuscript are in a public GitHub
   repository, https://github.com/khushal6001-maker/microsecond-readuntil. The software given
   in the Availability statement is that repository's release v1.0.0, archived at Zenodo as
   https://doi.org/10.5281/zenodo.23604395. [PREPRINT STATUS: e.g. "No preprint has been
   posted on a preprint server."]

**Use of AI tools.** The work was carried out with the assistance of Claude (Anthropic), used
through Claude Code (Claude Opus 5 and Claude Opus 5.5), and the repository's history records it:
every commit carries a Claude co-author line. It helped write the pipeline's source code and the
scripts that map and score reads and draw the figures, and it helped draft the dissertation
chapter from which the manuscript is developed. For the manuscript itself it drafted the text;
identified the references and checked each one against Crossref, the publisher's page or arXiv;
wrote the script that recomputes every number from the committed result files; typeset the LaTeX
source; and checked the chapter's numbers against the raw data, which led to several corrections.
It also ran two analyses on existing simulated reads: the three-seed replication of the RawHash2
dwell × window grid, and the matched operating points computed from that grid's output. No reads
were simulated for the manuscript. This use is disclosed in the manuscript's Acknowledgements and
documented in Supplementary Section S6. The authors directed the work, reviewed and edited the
text, checked its claims against the data, and take full responsibility for the content.

**Declarations.** This manuscript is not under consideration elsewhere, both authors have approved
its submission, and we have no competing interests to declare. [CONFIRM EACH BEFORE SENDING.]

Yours sincerely,

Khushal Singh (corresponding author), on behalf of both authors
Department of Computer Engineering, National Institute of Technology Kurukshetra
Kurukshetra, Haryana 136119, India
khushal6002@gmail.com
