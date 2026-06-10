cmake_minimum_required( VERSION 3.16 )

project( PulsarExamples LANGUAGES CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

# Collect Pulsar files
file( GLOB PULSAR_SOURCES
	${CMAKE_SOURCE_DIR}/../libs/pulsar/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar/include/kmac/pulsar/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar_extras/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar_extras/include/kmac/pulsar/*.h
)

# Collect Pulsar documents
file( GLOB PULSAR_DOCS
	${CMAKE_SOURCE_DIR}/../*.md
	${CMAKE_SOURCE_DIR}/../docs/*.md
)

add_executable( PulsarExamples
	example.cpp
	${PULSAR_SOURCES}
	${PULSAR_DOCS}
)

set( FILES_DIR ../libs/pulsar/include )
set( EXTRAS_DIR ../libs/pulsar_extras/include )
include_directories( ${FILES_DIR} ${EXTRAS_DIR} )

include( GNUInstallDirs )
install( TARGETS PulsarExamples
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
