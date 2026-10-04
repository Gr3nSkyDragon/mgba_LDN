include(ExportDirectory)
set(SOURCE_FILES
	audio.c
	cheats.c
	core.c
	gb.c
	input.c
	io.c
	linktrace.c
	mbc.c
	mbc/huc-3.c
	mbc/licensed.c
	mbc/mbc.c
	mbc/pocket-cam.c
	mbc/tama5.c
	mbc/unlicensed.c
	memory.c
	overrides.c
	serialize.c
	renderers/cache-set.c
	renderers/software.c
	sio.c
	timer.c
	video.c)

if(WIN32)
	list(APPEND SOURCE_FILES linktrace-win32.c)
endif()

set(SIO_FILES
	sio/lockstep.c
	sio/printer.c
	sio/uds-pia.c
	sio/uds-session.c
	sio/uds-room.c
	sio/uds-udp.c
	sio/uds-joiner.c)

set(EXTRA_FILES
	extra/proxy.c)

set(DEBUGGER_FILES
	debugger/cli.c
	debugger/debugger.c
	debugger/symbols.c)

set(TEST_FILES
	test/core.c
	test/gbx.c
	test/mbc.c
	test/memory.c
	test/rtc.c)

source_group("GB board" FILES ${SOURCE_FILES})
source_group("GB extras" FILES ${EXTRA_FILES} ${SIO_FILES})
source_group("GB debugger" FILES ${DEBUGGER_FILES})
source_group("GB tests" FILES ${TEST_FILES})

export_directory(GB SOURCE_FILES)
export_directory(GB_SIO SIO_FILES)
export_directory(GB_EXTRA EXTRA_FILES)
export_directory(GB_DEBUGGER DEBUGGER_FILES)
export_directory(GB_TEST TEST_FILES)
