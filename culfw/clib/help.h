#ifndef _HELP_H_
#define _HELP_H_

/* ?? lists the commands with a short description, ?<letter> describes one.
   A bare ? keeps its old answer, "? (? is unknown) Use one of ...": FHEM
   reads the command letters from it. */
void help(const char *in);

#endif
