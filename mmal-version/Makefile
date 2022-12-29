# apt install clang

pimera: src/main.c
	clang -O2 -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		src/main.c

debug: src/main.c
	clang -g -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		src/main.c
