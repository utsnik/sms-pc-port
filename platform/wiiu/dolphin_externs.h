/* Keep Dolphin's fixed-address state declarations external in the isolated
 * shared-provider target. GCC's public Dolphin types header otherwise emits
 * one tentative definition from every provider translation unit. */
#undef AT_ADDRESS
#define AT_ADDRESS(addr) __attribute__((weak))
