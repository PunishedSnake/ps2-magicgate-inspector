#ifndef MCI_MAGICGATE_SESSION_H
#define MCI_MAGICGATE_SESSION_H

/*
 * The security personality deliberately suppresses MCSERV. Raw card imaging is
 * the one controlled exception: it needs PS2SDK MCSERV's page RPCs. The switch
 * is EE-local and must be enabled only around loading the embedded MCSERV.
 */
void MciSessionAllowRealMcserv(int allowed);

#endif /* MCI_MAGICGATE_SESSION_H */
