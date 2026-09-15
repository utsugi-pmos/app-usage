# Built natively on the surya itself. That was the open question in task 018 and
# it is settled: the phone already carries cmake, g++ and qt6-qtbase-dev, has
# 65 GB free and 5.5 GB of RAM, and is aarch64 -- the target architecture, not a
# cross-compiled approximation of it.
#
#   ssh surya
#   cmake -S . -B build -G Ninja && cmake --build build -j4
#   ./build/try-collector 10          # what the sampler actually reads
#   ./build/app-usaged --informe      # the list, without a compositor
#   ./build/app-usage                 # the window
#
# Four artefacts out of two static libraries:
#
#   app-usage-collector   the sampler + the store (QtCore only)
#   app-usage-frontend    the three QML models, over the collector
#   app-usaged            the service: samples for days, never draws
#   app-usage             the window, in the app drawer... no longer: NoDisplay
#   kcm_app_usage.so      the same screen, inside System Settings > Hardware
#
# The splits are not decoration. The daemon is QtCore only and runs for days, so
# linking QtQuick into it would pull the whole graphics stack into a process
# that never draws. And the models live in their own library so the window and
# the KCM share one implementation and cannot drift apart -- the same shape as
# lost-phone and smart-unlock next door.

cmake_minimum_required(VERSION 3.20)
project(app-usage VERSION 1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(KF_MIN_VERSION 6.0)
set(QT_MIN_VERSION 6.5)

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# KDEInstallDirs for the KCM plugin path; KDECMakeSettings for the build layout.
# NOT KDECompilerSettings: it defines QT_NO_CAST_FROM_ASCII, and the daemon and
# collector here use bare string literals throughout -- turning that on would be
# a rewrite of code that has nothing to do with the KCM. The KCM's own sources
# use QStringLiteral, so they lose nothing.
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
	CoreAddons
	I18n
	KCMUtils
)

qt_standard_project_setup(REQUIRES ${QT_MIN_VERSION})

include(GNUInstallDirs)

# --- the sampler + the store (no UI) ----------------------------------------
add_library(app-usage-collector STATIC
	src/collector.cpp
	src/store.cpp
	src/appinfo.cpp
)
target_link_libraries(app-usage-collector PUBLIC Qt6::Core)
target_include_directories(app-usage-collector PUBLIC src)
set_property(TARGET app-usage-collector PROPERTY POSITION_INDEPENDENT_CODE ON)

# --- the three QML models, shared by the window and the KCM -----------------
# UsageModel reads the store; Launcher opens Plasma's Energy page; Calibrator
# runs the calibrate script. Both front-ends link this, which is what keeps them
# from drifting.
add_library(app-usage-frontend STATIC
	src/usage.cpp
	src/usage.h
	src/launcher.cpp
	src/launcher.h
	src/calibrator.cpp
	src/calibrator.h
)
target_link_libraries(app-usage-frontend PUBLIC
	app-usage-collector
	Qt6::Core
	Qt6::Gui
	Qt6::Qml
)
target_include_directories(app-usage-frontend PUBLIC src)
set_property(TARGET app-usage-frontend PROPERTY POSITION_INDEPENDENT_CODE ON)

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
	message(WARNING "lrelease not found: the interface will only come out in English")
endif()

# --- the System Settings module ----------------------------------------------
# kcmutils_add_qml_kcm() GLOB_RECURSEs ui/, so ui/main.qml, ui/Body.qml,
# ui/AppsPage.qml, ui/Ink.qml and ui/Format.qml all become the KCM's QML module
# and can see each other without an import. App.qml is NOT in ui/ on purpose: it
# is the window's, and would otherwise be swept in too.
#
# Ink and Format are singletons and CMake has to be told, or the module
# registers them as ordinary types and every `Ink.ink` in the QML is silently
# undefined. This must be set before the macro reads the ui/ files.
set_source_files_properties(
	${CMAKE_CURRENT_SOURCE_DIR}/ui/Ink.qml
	${CMAKE_CURRENT_SOURCE_DIR}/ui/Format.qml
	PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

kcmutils_add_qml_kcm(kcm_app_usage SOURCES kcm.cpp)
target_link_libraries(kcm_app_usage PRIVATE
	app-usage-frontend
	KF6::I18n
	KF6::KCMUtils
	KF6::KCMUtilsQuick
)

# --- the standalone window ---------------------------------------------------
qt_add_executable(app-usage src/ui.cpp)
target_link_libraries(app-usage PRIVATE
	app-usage-frontend
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

# Body/AppsPage/Ink/Format are shared with the KCM, but one .qml cannot belong
# to two QML modules: qt_add_qml_module stores the URI in source-file
# properties, so the KCM's copy and the app's would fight. Same fix as
# lost-phone -- a build-time copy for the app, with ui/ staying the one place
# to edit.
configure_file(ui/Body.qml     ${CMAKE_CURRENT_BINARY_DIR}/appqml/Body.qml     COPYONLY)
configure_file(ui/AppsPage.qml ${CMAKE_CURRENT_BINARY_DIR}/appqml/AppsPage.qml COPYONLY)
configure_file(ui/Ink.qml      ${CMAKE_CURRENT_BINARY_DIR}/appqml/Ink.qml      COPYONLY)
configure_file(ui/Format.qml   ${CMAKE_CURRENT_BINARY_DIR}/appqml/Format.qml   COPYONLY)

set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/appqml/Body.qml
	PROPERTIES QT_RESOURCE_ALIAS Body.qml)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/appqml/AppsPage.qml
	PROPERTIES QT_RESOURCE_ALIAS AppsPage.qml)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/appqml/Ink.qml
	PROPERTIES QT_RESOURCE_ALIAS Ink.qml QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/appqml/Format.qml
	PROPERTIES QT_RESOURCE_ALIAS Format.qml QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(app-usage
	URI AppUsage
	VERSION 1.0
	QML_FILES
		App.qml
		${CMAKE_CURRENT_BINARY_DIR}/appqml/Body.qml
		${CMAKE_CURRENT_BINARY_DIR}/appqml/AppsPage.qml
		${CMAKE_CURRENT_BINARY_DIR}/appqml/Ink.qml
		${CMAKE_CURRENT_BINARY_DIR}/appqml/Format.qml
)

# --- the diagnostics ---------------------------------------------------------
# Off in the package, on by default when working in the tree: they are never
# installed, so building them inside abuild would only cost time.
#
# They exist because compiling collector.cpp proved nothing about whether it
# reads the right files: every bug found on the first run -- and there were
# four -- was a silent wrong answer, not a compiler error.
option(APP_USAGE_DIAGNOSTICOS "Construir los arneses de diagnostico" ON)
if(APP_USAGE_DIAGNOSTICOS)
	add_executable(try-collector src/try-collector.cpp)
	target_link_libraries(try-collector PRIVATE app-usage-collector)

	add_executable(try-appinfo src/try-appinfo.cpp)
	target_link_libraries(try-appinfo PRIVATE app-usage-collector)

	add_executable(try-store src/try-store.cpp)
	target_link_libraries(try-store PRIVATE app-usage-collector)
endif()

# --- install -----------------------------------------------------------------
# kcm_app_usage.so is installed by kcmutils_add_qml_kcm() itself, into the KCM
# plugin dir, so it is not named here.
install(TARGETS app-usaged app-usage
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Phase 0. Without it an installed system could never produce the calibration
# the daemon reads, so the screen and system rows could never appear and the
# "not calibrated" note would be permanent. Renamed on the way in because
# `calibrate` alone says nothing about what it calibrates.
install(PROGRAMS calibrate
	DESTINATION ${CMAKE_INSTALL_BINDIR}
	RENAME app-usage-calibrate)

# NoDisplay=true in the file: battery usage lives in System Settings now, not in
# the app drawer. The window binary stays for --capture and direct launches.
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
# package outright. systemd only ever looks in /usr/lib/systemd/user anyway.
install(FILES app-usaged.service
	DESTINATION lib/systemd/user)
