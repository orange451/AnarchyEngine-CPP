/* miniaudio with stb_vorbis: stb_vorbis's declarations first, so miniaudio's
   Vorbis decoder is compiled in, then its definitions after. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#include "miniaudio.c"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
