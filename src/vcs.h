#ifndef VCS_H
#define VCS_H

int vcs_init_repo(void);
int vcs_status(void);
int vcs_commit(const char *message);
int vcs_log(void);
int vcs_checkout(const char *commit_prefix);

#endif // VCS_H
