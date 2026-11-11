FILES = src/main.c src/parse.c src/mach.c src/hashtable.c src/events.c src/windows.c src/border.c src/animation.c src/mission_control.c
LIBS = -framework AppKit -framework CoreVideo -F/System/Library/PrivateFrameworks/ -framework SkyLight

all: | bin
	clang -std=c99 -O3 -g $(FILES) -o bin/borders $(LIBS)

debug: | bin
	clang -std=c99 -O0 -g -DDEBUG $(FILES) -o bin/debug $(LIBS)

asan: | bin
	clang -std=c99 -Wall -g -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g $(FILES) -o bin/debug $(LIBS)
	./bin/debug

test: | bin
	clang -std=c99 -g tests/mission_control_test.c src/parse.c src/hashtable.c -o bin/mission-control-test $(LIBS)
	./bin/mission-control-test

install-local: all
	mkdir -p $(HOME)/.local/libexec/jankyborders
	install -m 755 bin/borders $(HOME)/.local/libexec/jankyborders/borders
	codesign --force --sign - --identifier local.jankyborders.mission-control $(HOME)/.local/libexec/jankyborders/borders

bin:
	mkdir bin

clean:
	rm -rf bin
