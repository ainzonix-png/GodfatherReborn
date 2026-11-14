#!/bin/bash
# usage: build.sh <module>   -> /tmp/rc/h/<module>/t
set -e
M=$1; O_SUFFIX=${2:-}; S=/home/claude/w/infamous-recomp/spu/$M; O=/tmp/rc/h/$M$O_SUFFIX; RT=/home/claude/w/ps3recomp/runtime/spu
mkdir -p $O/orig $O/rc
P=$(grep -o '^void [A-Za-z0-9_]*_spu_func_' $S/spu_recomp.c | head -1 | sed 's/^void //;s/_spu_func_$//')
sed 's#"../../runtime/spu/spu_helpers.h"#"spu_helpers.h"#' $S/spu_recomp.c > $O/orig/spu_recomp.c
cp $S/spu_recomp.h $O/orig/
sed "s#\"../../runtime/spu/spu_helpers.h\"#\"spu_helpers.h\"#; s/\b${P}_spu_/RC_${P}_spu_/g" ${RCSRC:-$S/spu_recomp_rc.c} > $O/rc/spu_recomp.c
sed "s/\b${P}_spu_/RC_${P}_spu_/g" $S/spu_recomp.h > $O/rc/spu_recomp.h
F="-O2 -msse4.1 -mavx2 -mfma -mbmi2 -DSPU_USE_DISPATCH_MT -I$RT -w"
clang -c $F $O/orig/spu_recomp.c -o $O/orig.o &
clang -c $F $O/rc/spu_recomp.c -o $O/rc.o &
wait
grep -o '^void [A-Za-z0-9_]*_spu_func_[0-9A-Fa-f_]*(spu_context' $S/spu_recomp.c | sed 's/^void //;s/(spu_context//' > $O/names.txt
{ echo '#include "spu_context.h"'; echo 'typedef void (*fn_t)(spu_context*);'
  while read n; do echo "void $n(spu_context*); void RC_$n(spu_context*);"; done < $O/names.txt
  echo 'fn_t T_orig[] = {'; sed 's/$/,/' $O/names.txt; echo '};'
  echo 'fn_t T_rc[] = {'; sed 's/^/RC_/;s/$/,/' $O/names.txt; echo '};'
  echo 'const char* T_name[] = {'; sed 's/^/"/;s/$/",/' $O/names.txt; echo '};'
  echo "int T_n = $(wc -l < $O/names.txt);"; } > $O/table.c
clang -O1 -w -I$RT -c $O/table.c -o $O/table.o
clang -O1 -w -I$RT -c /tmp/rc/h/driver.c -o $O/driver.o
clang $O/orig.o $O/rc.o $O/table.o $O/driver.o -lm -o $O/t
echo built $O/t $(wc -l < $O/names.txt) funcs
