#ifndef MCI_KELF_CACHE_H
#define MCI_KELF_CACHE_H

#include <tamtypes.h>

/*
 * The cache owns one immutable raw, unbound KELF in EE RAM. Callers never
 * receive the cached allocation directly: every consumer gets a disposable
 * aligned clone because SECRMAN/libsecr binding mutates Kbit/Kc/ICVPS2 fields.
 */
void MciKelfCacheInvalidate(void);
int MciKelfCacheGetSource(char *path, unsigned int path_size,
                          unsigned int *size);
int MciKelfCacheClone(const char *path, unsigned int expected_size,
                      unsigned char **out_data, unsigned int *out_size,
                      int *cache_hit);

#endif /* MCI_KELF_CACHE_H */
