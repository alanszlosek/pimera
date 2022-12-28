
main: main.c
	clang -O2 -o main \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		main.c

debug: main.c
	clang -g -o main \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		main.c