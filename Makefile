FILES=main.cpp filereader.cpp propellant.cpp ../libpropfile/libpropfile.a natives.cpp
all: linux
linux: clean
	make -C ../libpropfile
	g++ -g $(FILES) platforms/linux.cpp -std=c++20 -Wall -Werror -o out/thruster -fsanitize=address -fno-omit-frame-pointer -g
windows: clean
	x86_64-w64-mingw32-g++ $(FILES) platforms/windows.cpp -Wall -Werror -o out/thruster.exe
macos: clean
	darling shell clang++ -std=c++20 -arch x86_64 $(FILES) platforms/macos.cpp -Wall -Werror -o out/thruster_macos
macos_arm64:
	darling shell clang++ -std=c++20 -arch arm64 $(FILES) platforms/macos.cpp -Wall -Werror -o out/thruster_macos_aarch64
clean:
	rm -rf out || true
	mkdir out || true