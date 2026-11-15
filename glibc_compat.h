/* Linux port: the program built on a new system should start on one a few years older too. Two maths
 * functions got new versions in glibc 2.43; the old ones (there since 2.2.5) do the same for what's asked
 * of them here. With this the newest glibc needed is 2.38 (2023). x86-64 only. */
#if defined(__x86_64__) && defined(__linux__)
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
#endif
