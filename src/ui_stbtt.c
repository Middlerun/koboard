// stb_truetype lives here so that both the measurement code and the host
// preview rasteriser share one copy. FBInk keeps its own private copy, so
// there is no symbol clash on the device.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
