DOC = QSOE-Design

.PHONY: all clean

all:
	latexmk -pdf $(DOC).tex

clean:
	latexmk -C $(DOC).tex
	rm *.aux
