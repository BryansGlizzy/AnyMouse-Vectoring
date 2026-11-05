#!/bin/zsh
# Run every OpenMouse *.test.ts inside QuickJS-ng. Usage: run_all.sh <openmouse dir> <esbuild pkg dir> <out dir>
OM=$1; E=$2; OUT=$3
HERE=${0:A:h}
mkdir -p $OUT
R=$OUT/results.txt; : > $R
find $OM/src -name '*.test.ts' | sort | while read t; do
  rel=${t#$OM/src/}
  js=$OUT/${rel//\//_}.js
  if ! node $HERE/bundle_test.mjs $E $OM $t $js > $OUT/build.log 2>&1; then
    echo "BUILDFAIL $rel $(grep -m1 'ERROR' $OUT/build.log | cut -c1-100)" >> $R; continue
  fi
  res=$(perl -e 'alarm 120; exec @ARGV' qjs --std --module $js 2>&1)
  line=$(print -r -- "$res" | grep '^RESULT' | tail -1)
  if [[ -z $line ]]; then
    echo "CRASH $rel $(print -r -- "$res" | grep -v '^not ok' | head -3 | tr '\n' ' ' | cut -c1-220)" >> $R
  else
    echo "$line $rel" >> $R
    print -r -- "$res" | grep -A1 '^not ok' | sed "s|^|    [$rel] |" >> $OUT/failures.txt
  fi
done
awk '/^RESULT/{split($2,a,"=");split($3,b,"=");p+=a[2];f+=b[2];n++; if(b[2]==0)c++} END{printf "files run: %d (all-pass: %d)  tests pass: %d  fail: %d\n", n, c, p, f}' $R
echo "build failures: $(grep -c '^BUILDFAIL' $R)  crashes: $(grep -c '^CRASH' $R)"
