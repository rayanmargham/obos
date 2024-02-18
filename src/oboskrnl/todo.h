/*
	oboskrnl/todo.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

// pragma message works on both msvc and gcc, so we don't need to worry about that.

#define DO_PRAGMA(x) _Pragma(#x)
#define COMPILE_MESSAGE(msg) DO_PRAGMA(message (#msg))
#define TODO(message) DO_PRAGMA(message ("TODO: " #message))
#define FIXME(message) DO_PRAGMA(message ("FIXME: " #message))