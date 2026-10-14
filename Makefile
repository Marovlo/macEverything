CC := clang
CFLAGS := -std=c11 -O3 -Wall -Wextra -Werror -Isrc -Iinclude -D_DARWIN_C_SOURCE -fblocks
OBJCFLAGS := -fobjc-arc $(CFLAGS)
LDFLAGS := -framework CoreFoundation -framework CoreServices
APP_LDFLAGS := $(LDFLAGS) -framework Foundation -framework AppKit -framework Carbon -framework UniformTypeIdentifiers

CORE := src/util.c src/fold.c src/index.c src/search.c src/scan.c src/persist.c src/watch.c
CORE_OBJ := $(CORE:src/%.c=build/%.o)

.PHONY: all test app dmg clean

all: build/maceverything build/maceverything-test app

build/%.o: src/%.c include/me.h src/internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cli.o: src/cli.c include/me.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/tests.o: src/tests.c include/me.h src/internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/App.o: app/App.m include/me.h
	@mkdir -p build
	$(CC) $(OBJCFLAGS) -c $< -o $@

build/maceverything-test: $(CORE_OBJ) build/tests.o
	$(CC) -O3 -o $@ $^ $(LDFLAGS)

build/maceverything: $(CORE_OBJ) build/cli.o build/App.o
	$(CC) -O3 -o $@ $^ $(APP_LDFLAGS)

app: build/maceverything app/Info.plist
	mkdir -p build/MacEverything.app/Contents/MacOS
	cp build/maceverything build/MacEverything.app/Contents/MacOS/MacEverything
	cp app/Info.plist build/MacEverything.app/Contents/Info.plist
	mkdir -p build/MacEverything.app/Contents/Resources
	cp app/AppIcon.icns build/MacEverything.app/Contents/Resources/AppIcon.icns
	cp app/MenubarIcon.png build/MacEverything.app/Contents/Resources/MenubarIcon.png
	codesign --force --sign - --identifier com.marovlo.macEverything build/MacEverything.app

dmg: app
	rm -rf build/dmg build/MacEverything.dmg
	mkdir -p build/dmg
	cp -R build/MacEverything.app build/dmg/
	ln -s /Applications build/dmg/Applications
	hdiutil create -volname MacEverything -srcfolder build/dmg -ov -format UDZO build/MacEverything.dmg
	# 清理暂存的 .app 副本，避免 Spotlight 索引出重复应用
	rm -rf build/dmg build/MacEverything.app

test: build/maceverything-test app
	./build/maceverything-test
	rm -rf /tmp/me-smoke-root
	mkdir -p /tmp/me-smoke-root
	printf 'x\n' > /tmp/me-smoke-root/readme.md
	./build/maceverything scan /tmp/me-smoke-root -o /tmp/me-smoke.bin
	ME_SMOKE=1 ME_INDEX=/tmp/me-smoke.bin ME_QUERY=readme ./build/MacEverything.app/Contents/MacOS/MacEverything
	ME_INDEX=/tmp/me-smoke.bin ./build/MacEverything.app/Contents/MacOS/MacEverything & pid=$$!; \
	sleep 1; \
	kill -0 $$pid; \
	kill $$pid; \
	wait $$pid || true

clean:
	rm -rf build
