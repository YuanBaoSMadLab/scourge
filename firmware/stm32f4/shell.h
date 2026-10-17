#ifndef SHELL_H
#define SHELL_H

void shell_banner(void);
void shell_prompt(void);
void shell_exec(char* line);
int  shell_auto(void);

#endif
