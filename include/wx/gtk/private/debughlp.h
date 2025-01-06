#ifndef _WX_GTK_PRIVATE_DEBUGHLP_H_
#define _WX_GTK_PRIVATE_DEBUGHLP_H_

#include "wx/defs.h"

bool wxGtkDebugLog(const char* format, const char* function_name, int line_num, const char* src_file ...);

#define DO_GTK_DEBUG_LOG(format, ...) \
	wxGtkDebugLog(format, __func__, __LINE__, __FILE__, __VA_ARGS__)
	
#endif