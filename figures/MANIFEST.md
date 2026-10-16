# Generated figures

Produced by `scripts/generate_thesis_figures.py` from the files named
below and from nothing else. Regenerate after any correction to a
results file; do not edit a figure by hand.

| figure | source data | content |
|---|---|---|
| `fig4-1-decision-rules` | `results/decision-rules.csv` | The six decision rules with their across-seed ranges. |
| `fig5-1-latency-distribution` | `results/hgrm/{pinned,unpinned}-{2,4}_{1..4}.hgrm` | Decision-latency distributions for the four cells of the placement matrix. |
| `fig5-2-bucket-edge-artefact` | `constants quoted in §5.2.2 (no data file; the instrument was replaced)` | Every reported p99 coincides with a bucket boundary, so the agreement was an artefact. |
| `fig5-3-percentile-spread` | `results/latency-matrix-wsl2.csv` | Run-to-run spread of each percentile, by shard count and placement. |
| `fig5-4-r3-distribution` | `results/hgrm/r3_frozen_*.hgrm, results/r3-latency.csv` | Regime R3 latency across nine runs against the 1.56 ms service bound. |
| `fig6-1-dwell-crossover` | `results/dwell-sweep-ours.csv` | Fixed-width collapses and detection does not; the ranking inverts between CV 0 and 0.10. |
| `fig6-2-rawhash-dwell` | `results/rawhash-dwell-sweep.csv` | Correct and false mapping rates for the comparison mapper across the dwell sweep. |
| `fig6-3-minimizer-penalty` | `results/rawhash-dwell-sweep.csv` | Accuracy cost of minimizer sketching as a function of dwell dispersion. |
| `fig7-1-ae-surface` | `arithmetic, with operating points from results/agreement-summary.csv` | The a^E surface with each system's measured operating point marked. |
| `fig7-2-agreement-comparison` | `results/agreement-summary.csv` | Agreement, correspondence-failure rate and measured key match probability, both systems. |
| `fig7-3-model-vs-measured` | `results/agreement-summary.csv` | Measured key match probability against the a^E prediction, sized by indel rate. |
| `fig7-4-per-read-distributions` | `results/agreement-{our_detect,our_fixed,rh}_std*.csv` | Empirical CDFs of per-read agreement and indel fraction for four cells. |
| `fig7-5-geometry-x-chaining` | `results/geometry-x-chaining.csv` | Voting against chaining across key geometries; the decision rule moves little. |
| `fig7-6-key-entropy` | `results/key-entropy.csv` | Realised entropy and occurrence-cap survival by geometry, at both reference sizes. |
