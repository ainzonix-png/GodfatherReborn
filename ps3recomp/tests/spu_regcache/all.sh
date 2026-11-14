#!/bin/bash
cd /tmp/rc/h
for d in /home/claude/w/infamous-recomp/spu/*/spu_recomp_rc.c; do M=$(basename $(dirname $d)); echo $M; done > mods.txt
run() { M=$1; RCSRC=/tmp/rc/spu2/$M/spu_recomp_rc.c ./build.sh $M > $M.build.log 2>&1 && (cd $M && ./t 2 > ../$M.res 2>/dev/null); echo "$M $(tail -1 $M.res)"; }
export -f run
xargs -P 2 -I{} bash -c 'run {}' < mods.txt > all.log 2>&1
echo DONE >> all.log
