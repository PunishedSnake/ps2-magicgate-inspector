#ifndef MCI_MAGICGATE_SESSION_H
#define MCI_MAGICGATE_SESSION_H

/*
 * The security personality deliberately suppresses MCSERV. Raw card imaging is
 * the one controlled exception: it needs PS2SDK MCSERV's page RPCs. The switch
 * is EE-local and must be enabled only around loading the embedded MCSERV.
 */
void MciSessionAllowRealMcserv(int allowed);

/*
 * IOP resets do not reset these EE-side wrapper flags. Card Tools must clear
 * any synthetic MagicGate mcInit/mcSync state before binding a real MCSERV, or
 * the first raw mcInit can be swallowed by a stale fake session and subsequent
 * mcReadPage calls never acquire a live libmc command.
 */
void MciSessionResetShim(void);

#endif /* MCI_MAGICGATE_SESSION_H */
