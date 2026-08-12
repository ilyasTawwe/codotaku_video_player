// Single translation unit that instantiates the stb_truetype implementation.
// The header itself is vendored under external/stb/; everything else includes
// it without STB_TRUETYPE_IMPLEMENTATION to get only the declarations.
#define STB_TRUETYPE_IMPLEMENTATION
#include "external/stb/stb_truetype.h"
