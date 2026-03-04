/* Shim: prevent Qt's #define signals public from mangling GDBus struct field names */
#pragma push_macro("signals")
#undef signals
#include_next <gio/gdbusintrospection.h>
#pragma pop_macro("signals")
