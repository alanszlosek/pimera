# Packages required to build:
# apt install clang libcamera-dev libjpeg62-turbo-dev

pimera: main.cpp
	clang++ -g -std=c++17 -o pimera \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base -l jpeg -lpthread \
		main.cpp
