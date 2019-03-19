.PHONY: clean

all: clean grep.out true.out chars.out timeout.out verify.out complex.out random.out
	test "$$(cat grep.out)" = "bisect"
	test "$$(cat true.out)" = ""
	test "$$(tr -dc 't' < chars.out)" = "tttttttttttttttttttttttttttttttt"
	test "$$(cat timeout.out)" = ""
	test "$$(cat verify.out)" = "halfempty"
	test "$$(wc -c < complex.out)" -le 128

# Slower stress tests
stress: clean math.out math.in
	test "$$(bc < math.in)" -eq "$$(bc < math.out)"
	test "$$(wc -c < math.out)" -lt "$$(wc -c < math.in)""

verify.in:
	echo halfempty > $@

math.in:
	seq -8192 8192 | shuf | head -1024 | tr '\n' '+' | sed 's/$$/0/' > $@

%.in:
	shuf < /usr/share/dict/words > $@

%.out: %.sh %.in
	env G_DEBUG=gc-friendly G_SLICE=always-malloc ../halfempty $$(awk '/^# flag: / { print $$3 }' $<) -q -o $@ $+

clean:
	rm -f -- *.out *.in

