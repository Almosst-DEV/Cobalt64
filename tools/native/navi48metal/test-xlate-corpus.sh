#!/bin/zsh
# test-xlate-corpus.sh - T3 of NATIVE-S8-INPROC.md: the real reader-loading code (n48_xlate.h) with Apple's own LLVM (GPUCompiler.framework libLLVM, from the dyld shared cache) and with a second reader
# (Homebrew LLVM 23), and the real libn48xlate, over the shader corpus, against the daemon reference (pc-translate.py + the command-line tool + an always-pass validator). Reports identical / different / failed.
# host Mac only, arm64 (there is no Rosetta here): libn48xlate is the HOST build of the fork's n48xlate crate.
#   usage: test-xlate-corpus.sh [CORPUS_DIR]      CORPUS_DIR holds manifest.tsv, <sha>.air, <sha>.clang.ll and ref/ (made by xlate-corpus-prep.py + the reference pass); default $N48X_CORPUS
#   env: N48X_LIB (arm64 libn48xlate.dylib, default ~/navi48-native/metal2vulkan/target/release/libn48xlate.dylib), N48X_READER2 (second reader, default /opt/homebrew/opt/llvm@23/lib/libLLVM.dylib), N48X_TEXT=1 also compares the reader's text
#   Working files go in one mktemp dir under $TMPDIR (prefix-checked before removal).
set -u
D="$(cd "$(dirname "$0")" && pwd)"
CORPUS="${1:-${N48X_CORPUS:-}}"; [ -f "$CORPUS/manifest.tsv" ] && [ -d "$CORPUS/ref" ] || { echo "usage: $0 CORPUS_DIR (manifest.tsv, *.air, *.clang.ll, ref/)"; exit 2; }
LIB="${N48X_LIB:-$HOME/navi48-native/metal2vulkan/target/release/libn48xlate.dylib}"; [ -f "$LIB" ] || { echo "no $LIB (cargo build --release -p n48xlate in the fork)"; exit 2; }
R2="${N48X_READER2:-/opt/homebrew/opt/llvm@23/lib/libLLVM.dylib}"
W=$(mktemp -d "${TMPDIR:-/tmp}/xlatecorpus.XXXXXX"); case "$W" in */xlatecorpus.??????) ;; *) echo "bad scratch $W"; exit 2;; esac
trap 'case "$W" in */xlatecorpus.??????) rm -rf -- "$W";; esac' EXIT
cc -O1 -Wall -Wextra -Werror -I"$D" -o "$W/drv" "$D/test-xlate-corpus.c" || exit 2
export N48X_SPVCACHE="${N48X_SPVCACHE:-$D/spvcache}"
rc=0
echo "corpus: $(wc -l < "$CORPUS/manifest.tsv") inputs ($(cut -f3 "$CORPUS/manifest.tsv" | sort | uniq -c | tr '\n' ' '))"
for R in apple "$R2"; do
  [ "$R" = apple ] || [ -f "$R" ] || { echo "second reader $R not present: skipped"; continue; }
  tag=$([ "$R" = apple ] && echo apple || echo homebrew)
  mkdir -p "$W/out-$tag"
  "$W/drv" run "$CORPUS" "$W/out-$tag" "$LIB" "$R" || { echo "driver failed ($tag)"; rc=1; continue; }
  if [ "${N48X_TEXT:-0}" = 1 ]; then "$W/drv" ll "$CORPUS" "$W/ll-$tag" "$LIB" "$R" 2>/dev/null; python3 "$D/xlate-corpus-compare.py" "$CORPUS" "$W/out-$tag" "$W/ll-$tag" || rc=1
  else python3 "$D/xlate-corpus-compare.py" "$CORPUS" "$W/out-$tag" || rc=1; fi
done
exit $rc
