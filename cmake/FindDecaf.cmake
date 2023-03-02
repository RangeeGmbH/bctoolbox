############################################################################
# FindDecaf.cmake
# Copyright (C) 2017-2023  Belledonne Communications, Grenoble France
#
############################################################################
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
############################################################################
#
# - Find the decaf include files and library
#
#  DECAF_FOUND - system has lib decaf
#  DECAF_INCLUDE_DIRS - the decaf include directory
#  DECAF_LIBRARIES - The library needed to use decaf

if(TARGET decaf)
	set(DECAF_TARGET decaf)
elseif(TARGET decaf-static)
	set(DECAF_TARGET decaf-static)
endif()

if(DECAF_TARGET)

	# We are building decaf
	set(DECAF_LIBRARIES ${DECAF_TARGET})
	get_target_property(DECAF_INCLUDE_DIRS ${DECAF_TARGET} INTERFACE_INCLUDE_DIRECTORIES)


	include(FindPackageHandleStandardArgs)
	find_package_handle_standard_args(Decaf
		DEFAULT_MSG
		DECAF_INCLUDE_DIRS DECAF_LIBRARIES
	)

	mark_as_advanced(DECAF_INCLUDE_DIRS DECAF_LIBRARIES)

endif()
