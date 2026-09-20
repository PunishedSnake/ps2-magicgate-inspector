#ifndef MCI_CARD_HOT_SWAP_H
#define MCI_CARD_HOT_SWAP_H

/* Settle the Sony ROM X/XMCMAN view after a physical card replacement.
 * Returns 0 only for a formatted PS2 card. The first changed-card response is
 * treated as a transition rather than a final verdict. */
int MciNormalCardProbeFormatted(int port, int *free_clusters);

#endif /* MCI_CARD_HOT_SWAP_H */
