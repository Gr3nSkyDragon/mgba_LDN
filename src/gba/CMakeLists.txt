include(ExportDirectory)
set(SOURCE_FILES
	../gb/audio.c
	audio.c
	bios.c
	cart/ereader.c
	cart/gpio.c
	cart/matrix.c
	cart/unlicensed.c
	cart/vfame.c
	cheats.c
	cheats/codebreaker.c
	cheats/gameshark.c
	cheats/parv3.c
	core.c
	dma.c
	gba.c
	hle-bios.c
	input.c
	io.c
	memory.c
	overrides.c
	renderers/cache-set.c
	renderers/common.c
	renderers/gl.c
	renderers/software-bg.c
	renderers/software-mode0.c
	renderers/software-obj.c
	renderers/video-software.c
	savedata.c
	serialize.c
	sharkport.c
	sio.c
	sio/gbp.c
	timer.c
	video.c)

set(SIO_FILES
	sio/dolphin.c
	sio/lockstep.c
	sio/rfu.c
	sio/rfu-backends.c
	sio/rfu-udp.c
	sio/esp32/esp32-serial.c
	sio/esp32/esp32-wire.c
	sio/esp32/rfu-esp32.c)

if(USE_LDN_BROADCAST)
	list(APPEND SIO_FILES
		sio/ldn/ldnd.c
		sio/ldn/netlink.c
		sio/ldn/ldn.c
		sio/ldn/ldn-monitor.c
		sio/ldn/ldn-station.c
		sio/ldn/ldn-auth.c
		sio/ldn/ldn-pia.c
		sio/ldn/ldn-pia-reliable.c
		sio/ldn/ldn-pia-connect.c
		sio/ldn/rfu-broadcast.c
		../third-party/zstd/zstdlib.c)
endif()

set(EXTRA_FILES
	extra/battlechip.c
	extra/proxy.c)

set(DEBUGGER_FILES
	debugger/cli.c)

set(TEST_FILES
	test/cheats.c
	test/core.c)

source_group("GBA board" FILES ${SOURCE_FILES})
source_group("GBA extras" FILES ${EXTRA_FILES} ${SIO_FILES})
source_group("GBA debugger" FILES ${DEBUGGER_FILES})
source_group("GBA tests" FILES ${TEST_FILES})

export_directory(GBA SOURCE_FILES)
export_directory(GBA_SIO SIO_FILES)
export_directory(GBA_EXTRA EXTRA_FILES)
export_directory(GBA_DEBUGGER DEBUGGER_FILES)
export_directory(GBA_TEST TEST_FILES)
