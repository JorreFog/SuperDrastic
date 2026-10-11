#!/bin/sh
# hcmp.sh <tagA> <tagB>: compares the [hash] lines of two runs over the frames both have
cd /storage/dsflip/opt/runs
grep -a '\[hash\]' $1.log | cut -d' ' -f3- > /tmp/hA.txt; grep -a '\[hash\]' $2.log | cut -d' ' -f3- > /tmp/hB.txt
a=$(wc -l < /tmp/hA.txt); b=$(wc -l < /tmp/hB.txt); n=$a; [ $b -lt $n ] && n=$b
head -n $n /tmp/hA.txt > /tmp/hA2.txt; head -n $n /tmp/hB.txt > /tmp/hB2.txt
u=$(cut -d' ' -f2- /tmp/hA2.txt | sort -u | wc -l)
if [ $n -gt 0 ] && cmp -s /tmp/hA2.txt /tmp/hB2.txt; then echo "IDENTICAL ($n frames compared, $u distinct pictures)"
else d=$(diff /tmp/hA2.txt /tmp/hB2.txt | grep -c '^<'); echo "DIFFER ($d of $n frames, $u distinct; first: $(diff /tmp/hA2.txt /tmp/hB2.txt | grep -m1 '^<' | cut -c1-30) / $(diff /tmp/hA2.txt /tmp/hB2.txt | grep -m1 '^>' | cut -c1-60))"; fi
