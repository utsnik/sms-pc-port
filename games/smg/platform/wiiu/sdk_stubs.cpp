#include "sdk_stubs.h"

#include <whb/log.h>

#include <cstring>

namespace {

const unsigned kMaxLoggedStubs = 64;
const unsigned kNameBytes = 32;
char s_logged[kMaxLoggedStubs][kNameBytes];
unsigned s_logged_count;

} // namespace

extern "C" void smg_stage1_stub_log_once(const char* symbol)
{
  for (unsigned i = 0; i < s_logged_count; ++i) {
    if (std::strncmp(s_logged[i], symbol, kNameBytes) == 0)
      return;
  }
  if (s_logged_count < kMaxLoggedStubs) {
    std::strncpy(s_logged[s_logged_count], symbol, kNameBytes - 1);
    s_logged[s_logged_count][kNameBytes - 1] = '\0';
    ++s_logged_count;
  }
  WHBLogPrintf("SMG Wii U: Stage 1 stub %s (safe no-op)", symbol);
}

/* KPAD/WPAD do not belong to this stub boundary; input.cpp provides them.
 *
 * These entry points deliberately use a C varargs ABI.  The Revolution SDK
 * declarations live in the Petari headers and the functions are only link
 * boundaries in Stage 1; no stub may be called before a later service owner
 * is implemented.  Returning zero is the documented harmless failure for
 * this no-data boundary and is also safe for ignored void returns.
 */
#define SMG_DEFINE_STAGE1_STUB(symbol) \
  extern "C" int symbol(...) \
  { \
    smg_stage1_stub_log_once(#symbol); \
    return 0; \
  }

SMG_STAGE1_STUB_SYMBOLS(SMG_DEFINE_STAGE1_STUB)

#undef SMG_DEFINE_STAGE1_STUB
