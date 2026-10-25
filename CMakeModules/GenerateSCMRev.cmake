# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

# VIZR: re-run configure when HEAD moves, so the logged branch + hash never go stale. The git directory comes from git
# itself: in a worktree .git is a file and the worktree's HEAD and logs/HEAD live in the main repository.
set(_scm_git_dir "${CMAKE_SOURCE_DIR}/.git")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE _scm_git_out OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _scm_git_rc ERROR_QUIET)
    if (_scm_git_rc EQUAL 0 AND IS_DIRECTORY "${_scm_git_out}")
        set(_scm_git_dir "${_scm_git_out}")
    endif()
endif()
foreach(_scm_dep HEAD logs/HEAD)
    if (EXISTS "${_scm_git_dir}/${_scm_dep}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_scm_git_dir}/${_scm_dep}")
    endif()
endforeach()

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

set(GIT_DESC ${BUILD_VERSION})

# Generate cpp with Git revision from template

# TODO(crueter): Stable releases feed.
set(BUILD_AUTO_UPDATE_STABLE_REPO "eden-emu/eden")
set(BUILD_AUTO_UPDATE_STABLE_API "git.eden-emu.dev")
set(BUILD_AUTO_UPDATE_STABLE_API_PATH "/api/v1/repos/")

set(BUILD_AUTO_UPDATE_API_PATH "/latest/release.json")
if (NIGHTLY_BUILD)
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "nightly.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-ci/nightly")
    set(REPO_NAME "Eden Nightly")
else()
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "stable.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-emu/eden")
    set(REPO_NAME "Eden")
endif()

if (VIZR_OFFLINE)
    # VIZR: no release or update host in the binary (the requests that would use them are compiled out too).
    set(BUILD_AUTO_UPDATE_STABLE_API "")
    set(BUILD_AUTO_UPDATE_STABLE_API_PATH "")
    set(BUILD_AUTO_UPDATE_API_PATH "")
    set(BUILD_AUTO_UPDATE_WEBSITE "")
    set(BUILD_AUTO_UPDATE_API "")
endif()

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)
