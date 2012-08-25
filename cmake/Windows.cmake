# Copyright (c) 2011, Peter Kümmel
# All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.


if(MSVC)
    if(NOT PYTHONINTERP_FOUND OR NOT openssl)
        include(WindowsDependencies)
    endif()
    if(NOT openssl)
        message(FATAL_ERROR "'openssl' not set, install 'Win32OpenSSL' and set openssl")
    endif()
    include_directories(${openssl}/include)
    set(_ldir ${openssl}/lib/VC)
    set(libopenssl debug ${_ldir}/libeay32MDd.lib optimized ${_ldir}/libeay32MD.lib)
    if(MSVC_IDE)
        set(bin_sub_dir "\$(Configuration)")
    endif()
    set(warn_level 1)
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W${warn_level}" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "/EHsc")
        string(REGEX REPLACE "/Esc" "/Esc-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif()
    #set(warn_error   "/we4101")
    set(warn_disable "/wd4018 /wd4099 /wd4100 /wd4127 /wd4146 /wd4189 /wd4200 /wd4244 /wd4245 /wd4291 /wd4389 /wd4512 /wd4611 /wd4701 /wd4702 /wd4731 /wd4706 /wd4800")
    set(warn "${warn_disable} ${warn_error}")
    set(lang  "/GR- /GS /Zc:wchar_t")
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE)
    # disable checked iterators for msvc release builds to get maximum speed
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /D_SECURE_SCL=0")
    set(librt ws2_32 Rpcrt4)
endif()
