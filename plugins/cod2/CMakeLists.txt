# Copyright 2020-2021 The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_library(cod2 SHARED
	"cod2.cpp"

	"../Module.cpp"
	"../ProcessBase.cpp"
	"../ProcessWindows.cpp"
)

if(WIN32)
	target_sources(cod2 PRIVATE "../HostWindows.cpp")
else()
	target_sources(cod2 PRIVATE "../HostLinux.cpp")
endif()
