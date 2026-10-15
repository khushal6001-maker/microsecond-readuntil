// bench/symbol_dump.cpp
//
// Dump this pipeline's per-event quantised symbols, reference side and query side, in the
// same format bench/symbol_agreement.cpp consumes.
//
// The point of dumping rather than computing agreement in place is estimator symmetry.
// Chapter 7 section 7.4 measured agreement with a diagnostic built into the sweep, under a
// constant-offset correspondence. An external mapper's agreement cannot be measured that
// way, because its event retention is data-dependent. Emitting symbols from both systems
// in one format and scoring them with one program is the only way the two numbers are
// comparable; an agreement figure computed two different ways is not a comparison.
//
// The reference side here emits ONE EVENT PER K-MER, with no retention filter, because
// that is what PoreModel::reference_to_events does. The query side under event detection
// emits roughly 0.78 events per k-mer. That rate mismatch is a property of the pipeline,
// not of this tool, and it is visible in the indel fraction the scorer reports.
//
// Usage:
//   mru_symbol_dump <model.tsv> <ref.fa> <reads.slow5> <contig> <detect 0|1>
//                   <bits> <ref_out> <qry_out> [max_reads] [scale_from_first_chunk 0|1]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "index/quantise.hpp"

namespace {

int base_code(char c) {
  switch (c) {
    case 'A': case 'a': return 0;
    case 'C': case 'c': return 1;
    case 'G': case 'g': return 2;
    case 'T': case 't': return 3;
    default: return -1;
  }
}

bool load_model_tsv(const std::string& path, std::vector<float>& levels) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    levels.push_back(std::strtof(line.c_str() + tab + 1, nullptr));
  }
  std::size_t p = 1;
  while (p < levels.size()) p *= 4;
  return !levels.empty() && p == levels.size();
}

bool load_fasta_acgt(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      if (base_code(c) >= 0) out.push_back(c);
    }
  }
  return !out.empty();
}

std::size_t load_slow5(const std::string& path,
                       std::vector<std::vector<std::int16_t>>& reads,
                       std::vector<std::string>& ids, std::size_t limit) {
  std::ifstream f(path);
  if (!f) return 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line[0] == '@') continue;
    const char* p = line.c_str();
    int field = 0;
    const char* sig = nullptr;
    for (const char* q = p;; ++q) {
      if (*q == '\t' || *q == '\0') {
        ++field;
        if (field == 7) {
          sig = q + 1;
          break;
        }
        if (*q == '\0') break;
      }
    }
    if (sig == nullptr) continue;
    std::vector<std::int16_t> raw;
    raw.reserve(16384);
    const char* q = sig;
    while (*q != '\0' && *q != '\t') {
      raw.push_back(static_cast<std::int16_t>(std::strtol(q, nullptr, 10)));
      while (*q != '\0' && *q != ',' && *q != '\t') ++q;
      if (*q == ',') ++q;
    }
    if (!raw.empty()) {
      ids.push_back(line.substr(0, line.find('\t')));
      reads.push_back(std::move(raw));
    }
    if (limit != 0 && reads.size() >= limit) break;
  }
  return reads.size();
}

void revcomp(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (std::size_t i = in.size(); i-- > 0;) {
    switch (in[i]) {
      case 'A': case 'a': out.push_back('T'); break;
      case 'C': case 'c': out.push_back('G'); break;
      case 'G': case 'g': out.push_back('C'); break;
      default: out.push_back('A'); break;
    }
  }
}

void write_ref_stream(std::FILE* f, const char* contig, char strand,
                      const std::vector<mru::QEvent>& ev) {
  std::fprintf(f, ">%s\t%c\n", contig, strand);
  for (std::size_t i = 0; i < ev.size(); ++i) {
    std::fprintf(f, i == 0 ? "%zu:%u" : ",%zu:%u", i,
                 static_cast<unsigned>(ev[i]));
  }
  std::fputc('\n', f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 9) {
    std::fprintf(stderr,
                 "usage: %s <model.tsv> <ref.fa> <reads.slow5> <contig> <detect 0|1> "
                 "<bits> <ref_out> <qry_out> [max_reads] [scale_from_first_chunk 0|1]\n",
                 argv[0]);
    return 2;
  }
  const std::string model_path = argv[1];
  const std::string ref_path = argv[2];
  const std::string slow5_path = argv[3];
  const std::string contig = argv[4];
  const bool detect = std::atoi(argv[5]) != 0;
  const std::uint32_t bits = static_cast<std::uint32_t>(std::atoi(argv[6]));
  const std::string ref_out = argv[7];
  const std::string qry_out = argv[8];
  const std::size_t max_reads = (argc > 9) ? std::strtoul(argv[9], nullptr, 10) : 0;
  const bool scale_first_chunk = (argc > 10) ? (std::atoi(argv[10]) != 0) : false;

  std::vector<float> levels;
  if (!load_model_tsv(model_path, levels)) {
    std::fprintf(stderr, "model load failed\n");
    return 1;
  }
  std::string dna;
  if (!load_fasta_acgt(ref_path, dna)) {
    std::fprintf(stderr, "fasta load failed\n");
    return 1;
  }

  mru::QuantConfig cfg;
  cfg.event_detection = detect;
  cfg.minimizer_window = 1;
  if (bits != 0) cfg.bits_per_event = bits;
  if (!cfg.valid()) {
    std::fprintf(stderr, "invalid config\n");
    return 1;
  }

  mru::PoreModel model;
  if (!model.load_levels(levels)) {
    std::fprintf(stderr, "model levels failed\n");
    return 1;
  }

  // Forward and reverse encoded SEPARATELY, so that each stream's positions are indices
  // into that strand's own event array. squig_sweep concatenates them into one string for
  // indexing, which is correct there and would make the coordinate mapping here ambiguous.
  std::FILE* rf = std::fopen(ref_out.c_str(), "w");
  if (rf == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", ref_out.c_str());
    return 1;
  }
  {
    std::vector<mru::QEvent> ev;
    if (!model.reference_to_events(dna, cfg, ev)) {
      std::fprintf(stderr, "reference_to_events failed (forward)\n");
      return 1;
    }
    std::fprintf(stderr, "reference forward events: %zu for %zu bases\n", ev.size(),
                 dna.size());
    write_ref_stream(rf, contig.c_str(), '+', ev);
  }
  {
    std::string rc;
    revcomp(dna, rc);
    std::vector<mru::QEvent> ev;
    if (!model.reference_to_events(rc, cfg, ev)) {
      std::fprintf(stderr, "reference_to_events failed (reverse)\n");
      return 1;
    }
    write_ref_stream(rf, contig.c_str(), '-', ev);
  }
  std::fclose(rf);

  std::vector<std::vector<std::int16_t>> reads;
  std::vector<std::string> ids;
  if (load_slow5(slow5_path, reads, ids, max_reads) == 0) {
    std::fprintf(stderr, "no reads\n");
    return 1;
  }
  std::fprintf(stderr, "reads: %zu\n", reads.size());

  std::FILE* qf = std::fopen(qry_out.c_str(), "w");
  if (qf == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", qry_out.c_str());
    return 1;
  }

  // 0.4 s at squigulator's dna-r10-prom sampling rate of 5 kHz, the chunk the production
  // path normalises from.
  constexpr std::size_t kChunkSamples = 2000;

  mru::ScalingScratch sscratch;
  mru::EventScratch escratch;
  std::vector<float> means;
  std::vector<mru::QEvent> qev;
  std::size_t total_events = 0;

  for (std::size_t r = 0; r < reads.size(); ++r) {
    const std::vector<std::int16_t>& raw = reads[r];
    const std::size_t n_scale =
        scale_first_chunk ? std::min(raw.size(), kChunkSamples) : raw.size();
    const mru::SignalScaling sc = mru::scaling_from_samples(
        std::span<const std::int16_t>(raw.data(), n_scale), sscratch);
    if (!sc.valid()) continue;

    qev.clear();
    if (detect) {
      mru::quantise_signal_detected(std::span<const std::int16_t>(raw), sc, cfg, escratch,
                                    means, qev);
    } else {
      mru::quantise_signal(std::span<const std::int16_t>(raw), sc, cfg, qev);
    }
    if (qev.empty()) continue;
    total_events += qev.size();

    std::fprintf(qf, "%s\n", ids[r].c_str());
    for (std::size_t i = 0; i < qev.size(); ++i) {
      std::fprintf(qf, i == 0 ? "%u" : ",%u", static_cast<unsigned>(qev[i]));
    }
    std::fputc('\n', qf);
  }
  std::fclose(qf);
  std::fprintf(stderr, "mean events/read: %.0f  (detect=%d, bits=%u, scale=%s)\n",
               static_cast<double>(total_events) / static_cast<double>(reads.size()),
               detect ? 1 : 0, cfg.bits_per_event,
               scale_first_chunk ? "first-chunk" : "whole-read");
  return 0;
}
