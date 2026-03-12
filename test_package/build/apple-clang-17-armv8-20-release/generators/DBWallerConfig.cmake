########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(DBWaller_FIND_QUIETLY)
    set(DBWaller_MESSAGE_MODE VERBOSE)
else()
    set(DBWaller_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/DBWallerTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${dbwaller_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(DBWaller_VERSION_STRING "0.1.0")
set(DBWaller_INCLUDE_DIRS ${dbwaller_INCLUDE_DIRS_RELEASE} )
set(DBWaller_INCLUDE_DIR ${dbwaller_INCLUDE_DIRS_RELEASE} )
set(DBWaller_LIBRARIES ${dbwaller_LIBRARIES_RELEASE} )
set(DBWaller_DEFINITIONS ${dbwaller_DEFINITIONS_RELEASE} )


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${dbwaller_BUILD_MODULES_PATHS_RELEASE} )
    message(${DBWaller_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()


