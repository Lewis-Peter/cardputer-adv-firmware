#include <stddef.h>
#include <stdint.h>

extern "C" {
void* ssh_new(void) { return (void*)1; }
void ssh_free(void* s) { (void)s; }
int ssh_connect(void* s) { (void)s; return -1; }
void ssh_disconnect(void* s) { (void)s; }
int ssh_options_set(void* s, int t, const void* v) { (void)s; (void)t; (void)v; return 0; }
const char* ssh_get_error(void* s) { (void)s; return ""; }
int ssh_get_fd(void* s) { (void)s; return -1; }
void* ssh_channel_new(void* s) { (void)s; return (void*)1; }
int ssh_channel_open_session(void* c) { (void)c; return -1; }
int ssh_channel_close(void* c) { (void)c; return 0; }
void ssh_channel_free(void* c) { (void)c; }
int ssh_channel_request_pty_size(void* c, const char* t, int col, int row) { (void)c; (void)t; (void)col; (void)row; return -1; }
int ssh_channel_change_pty_size(void* c, int col, int row) { (void)c; (void)col; (void)row; return -1; }
int ssh_channel_request_shell(void* c) { (void)c; return -1; }
int ssh_channel_read_timeout(void* c, void* d, uint32_t len, int is_stderr, int to) { (void)c; (void)d; (void)len; (void)is_stderr; (void)to; return 0; }
int ssh_channel_write(void* c, const void* d, uint32_t len) { (void)c; (void)d; (void)len; return 0; }
int ssh_channel_send_eof(void* c) { (void)c; return 0; }
int ssh_channel_is_eof(void* c) { (void)c; return 1; }
int ssh_channel_is_open(void* c) { (void)c; return 0; }
int ssh_userauth_none(void* s, const char* u) { (void)s; (void)u; return -1; }
int ssh_userauth_password(void* s, const char* u, const char* p) { (void)s; (void)u; (void)p; return -1; }
int ssh_userauth_publickey(void* s, const char* u, void* k) { (void)s; (void)u; (void)k; return -1; }
int ssh_userauth_kbdint(void* s, const char* u, const char* sub) { (void)s; (void)u; (void)sub; return -1; }
int ssh_userauth_kbdint_setanswer(void* s, int i, const char* a) { (void)s; (void)i; (void)a; return -1; }
int ssh_pki_import_privkey_base64(const char* b64, const char* p, void* auth, void* priv, void** key) { (void)b64; (void)p; (void)auth; (void)priv; (void)key; return -1; }
void ssh_key_free(void* k) { (void)k; }
int ssh_init(void) { return 0; }
int ssh_finalize(void) { return 0; }
}

void libssh_begin() {}
