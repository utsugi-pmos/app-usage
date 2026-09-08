# Built natively on the surya itself. That was the open question in task 018 and
# it is settled: the phone already carries cmake, g++ and qt6-qtbase-dev, has
# 65 GB free and 5.5 GB of RAM, and is aarch64 -- the target architecture, not a
# cross-compiled approximation of it. Neither pmbootstrap nor a Qt6 install on
# the laptop is needed to build or to test.
#
#   ssh surya
#   cmake -S . -B build && cmake --build build -j4
#   ./build/probar-collector 10       # what the sampler actually reads
#   ./build/app-usaged --informe      # the list, without a compositor
#   ./build/app-usage                 # the screen
#
# Three binaries out of one static library, and the split matters: the daemon is
# QtCore only and runs for days, so linking QtQuick into it would pull the whole
# graphics stack into a process that never draws and make the thing whose job is
# to be cheap the biggest resident on the list it prints.

cmake_minimum_required(VERSION 3.20)
project(app-usage VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
add_compile_options(-Wall -Wextra)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)

include(GNUInstallDirs)

add_library(app-usage-collector STATIC
	src/collector.cpp
	src/store.cpp
	src/appinfo.cpp
)
target_link_libraries(app-usage-collector PUBLIC Qt6::Core)
target_include_directories(app-usage-collector PUBLIC src)
set_property(TARGET app-usage-collector PROPERTY POSITION_INDEPENDENT_CODE ON)

# --- the service, and the report that reads what it wrote --------------------
add_executable(app-usaged src/main.cpp)
target_link_libraries(app-usaged PRIVATE app-usage-collector)

# --- translations ------------------------------------------------------------
# Driven by calling lrelease directly rather than through qt_add_translations(),
# because this phone has qt6-qttools (the programs) without its -dev package
# (the CMake module). Needing the tool is fine; needing a CMake package that is
# not installed would mean the app could not be built on the only machine that
# can build it.
find_program(LRELEASE NAMES lrelease6 lrelease-qt6 lrelease)
set(TS_FILES i18n/app-usage_es.ts)
set(QM_FILES)
if(LRELEASE)
	foreach(ts ${TS_FILES})
		get_filename_component(base ${ts} NAME_WE)
		set(qm ${CMAKE_CURRENT_BINARY_DIR}/${base}.qm)
		add_custom_command(
			OUTPUT ${qm}
			COMMAND ${LRELEASE} -silent ${CMAKE_CURRENT_SOURCE_DIR}/${ts} -qm ${qm}
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${ts}
			COMMENT "lrelease ${ts}"
			VERBATIM)
		list(APPEND QM_FILES ${qm})
	endforeach()
else()
	message(WARNING "lrelease no encontrado: la interfaz saldra solo en ingles")
endif()

# --- the screen --------------------------------------------------------------
qt_add_executable(app-usage
	src/ui.cpp
	src/usage.cpp
	src/usage.h
	src/launcher.cpp
	src/launcher.h
	src/calibrator.cpp
	src/calibrator.h
)
target_link_libraries(app-usage PRIVATE
	app-usage-collector
	Qt6::Gui
	Qt6::Qml
	Qt6::Quick
)

if(QM_FILES)
	# Embedded rather than installed beside the binary: one file to ship, and no
	# way for the program and its translations to get out of step.
	qt_add_resources(app-usage "translations"
		PREFIX "/i18n"
		BASE ${CMAKE_CURRENT_BINARY_DIR}
		FILES ${QM_FILES})
endif()

# Tinta and Format are singletons and CMake has to be told, or the module
# registers them as ordinary types and every `Tinta.ink` in the QML is silently
# undefined. Tinta is NOT called Palette: QtQuick has owned that name since 6.6
# and would win the lookup, turning the whole screen into "Unable to assign
# [undefined]".
set_source_files_properties(Tinta.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(Format.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(app-usage
	URI AppUsage
	VERSION 1.0
	QML_FILES
		main.qml
		AppsPage.qml
		Tinta.qml
		Format.qml
)

# --- the diagnostics ---------------------------------------------------------
# Off in the package, on by default when working in the tree: they are never
# installed, so building them inside abuild would only cost time, and shipping
# their sources would put files in the recipe that nothing in the recipe uses.
#
# They exist because compiling collector.cpp proved nothing about whether it
# reads the right files: every bug found on the first run -- and there were
# four -- was a silent wrong answer, not a compiler error.
option(APP_USAGE_DIAGNOSTICOS "Construir los arneses de diagnostico" ON)
if(APP_USAGE_DIAGNOSTICOS)
	add_executable(probar-collector src/probar-collector.cpp)
	target_link_libraries(probar-collector PRIVATE app-usage-collector)

	# Checks the drawer/background split against units read off this phone's
	# live cgroups. Needs the phone's own .desktop files, so it exits 77
	# (skipped) anywhere else rather than reporting a failure it cannot judge.
	add_executable(probar-appinfo src/probar-appinfo.cpp)
	target_link_libraries(probar-appinfo PRIVATE app-usage-collector)

	# The store, and above all purge(): the only function here that deletes the
	# user's data. Builds its own directory in /tmp, so it runs anywhere.
	add_executable(probar-store src/probar-store.cpp)
	target_link_libraries(probar-store PRIVATE app-usage-collector)
endif()

# --- install -----------------------------------------------------------------
install(TARGETS app-usaged app-usage
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Phase 0. Without it an installed system could never produce the calibration
# the daemon reads, so the screen and system rows could never appear and the
# "not calibrated" note would be permanent. Renamed on the way in because
# `calibrar` alone says nothing about what it calibrates.
install(PROGRAMS calibrar
	DESTINATION ${CMAKE_INSTALL_BINDIR}
	RENAME app-usage-calibrar)

install(FILES app-usage.desktop
	DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES app-usage.svg
	DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# The user service, installed disabled. `systemctl --user enable` is the setup
# script's job, so that installing the package never starts sampling behind the
# user's back.
#
# The path is literal 'lib/', NOT ${CMAKE_INSTALL_LIBDIR}. Under a plain QEMU
# build KDEInstallDirs resolves that to lib64, and abuild then refuses the
# package outright ("must not put anything under /usr/lib64"). systemd only ever
# looks in /usr/lib/systemd/user anyway, and Alpine has no lib64, so hardcoding
# it is both correct and the thing that builds either way.
install(FILES app-usaged.service
	DESTINATION lib/systemd/user)
