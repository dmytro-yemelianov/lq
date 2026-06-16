DOC = seL4-Essentials

.PHONY: all clean

all:
	latexmk -pdf $(DOC).tex

clean:
	latexmk -C $(DOC).tex
	rm *.aux
