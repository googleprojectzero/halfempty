.PHONY: clean

all: clean grep.out true.out chars.out timeout.out verify.out complex.out random.out
	test "$$(cat grep.out)" = "bisect"
	test "$$(cat true.out)" = ""
	test "$$(tr -dc 't' < chars.out)" = "tttttttttttttttttttttttttttttttt"
	test "$$(cat timeout.out)" = ""
	test "$$(cat verify.out)" = "halfempty"
	test "$$(wc -c < complex.out)" -le 128

verify.in:
	echo halfempty > $@

%.in:
	shuf < /usr/share/dict/words > $@

%.out: %.sh %.in
	../halfempty $$(awk '/^# flag: / { print $$3 }' $<) -q -o $@ $+

clean:
	rm -f -- *.out *.in

